-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_About
-- kosmos: name Get Info
-- kosmos: section none
-- kosmos: needs screen
-- Info: what a file or a folder is, where, and how much (`roadmap.md` 6za).
--
--   info /Home/roms
--   info /Home/a.txt
--   /Home/b.sfc                    several, one path a line
--
-- Opened by Tracker's right click - Info on a file, a folder, several, a
-- place or the window itself - as `docs/rightclick.html` draws it and Diego
-- asked on the M700: "info (a new ui that displays basic info of the folder
-- like size and amount of files, date modified, etc)".
--
-- **It opens at once and counts after.** What the name and the kind are is
-- one question each; what a folder holds is a walk, and a walk of a drive is
-- thousands of questions. So the window draws what it knows, and
-- `/Kosmos/Libraries/tally.lua` counts a slice every pass while the numbers
-- climb - a window that walked first would be a window that did not open.
--
-- **Modified is the date the disk kept** (6za step b): a file in `/Home`
-- written since the disk server was given the clock says when, in local
-- time; one written before - stamped with a count since some boot - and a
-- file on a store that keeps no dates say nothing rather than a wrong one.

local ui       = use("/Kosmos/Libraries/ui.lua")
local files    = use("/Kosmos/Libraries/files.lua")
local types    = use("/Kosmos/Libraries/filetypes.lua")
local placelib = use("/Kosmos/Libraries/places.lua")
local tally    = use("/Kosmos/Libraries/tally.lua")
local clock    = use("/Kosmos/Libraries/clock.lua")

local theme = ui.theme

-- The paths, as Tracker writes them: each a word, quoted when it has a
-- space (`files.quote`). They were one a line, which only Tracker spoke.
local paths = files.words(args)

if #paths == 0 then
  print("usage: info <path> [<path> ...], a path with a space in quotes")
  return
end

local one = (#paths == 1) and paths[1] or nil
local attrs = one and fs.getattr(one) or nil

if one and not attrs then
  print("info: no " .. one)
  return
end

local name = one and (one:match("([^/]+)$") or one)
             or (#paths .. " items")

--
-- Where they are: the folder that holds it, or them when they share one.
--
local function where_of()
  local parent = files.parent(paths[1])

  for i = 2, #paths do
    if files.parent(paths[i]) ~= parent then return "in several places" end
  end

  return parent
end

--
-- **The drawing's measurements**, `docs/rightclick.html`'s Info: 348 wide,
-- 18 in from every side; the picture and the name with 14 between them and
-- 16 below; then one card - the kit's own rounded, sunken card, so it is
-- every other card in every look - whose rows are 9 and 13 in, at least
-- 40 tall, a name column 82 wide in the dim face, and 12 before the value.
-- Layout pixels are the drawing's, one for one; a face is 1.30 times its
-- CSS size (`theme.lua`), which is why every word here is in a role and
-- none has a size of its own.
--
local W = 348
local PAD, GAP, BELOW = 18, 14, 16
local PICTURE = 48
local ROW_IN, ROW_PAD, ROW_MIN = 13, 9, 40
local LABEL_W, LABEL_GAP = 82, 12

local folder = one and attrs.kind == "directory"
local counting = (not one) or folder
local count = counting and tally.new(fs, paths) or nil

-- A count as the drawing writes it: 2,007,961,344 bytes, 1,204 files.
local grouped = use("/Kosmos/Libraries/text.lua").grouped

local function plural(n, word)
  return ("%s %s%s"):format(grouped(n), word, n == 1 and "" or "s")
end

--
-- What it is, under its name: "Folder", or a kind of file with its
-- extension beside it when the kind has words - "Film, MP4" - so both the
-- word and the thing a person would search for are there.
--
local function kind_line()
  if not one then return "Several things" end

  local words = types.describe(one, attrs)
  local ext = attrs.kind ~= "directory" and attrs.kind ~= "launcher"
              and types.kind_of(one, attrs)

  if ext and types.names[ext] then
    return words .. ", " .. ext:upper()
  end

  return words
end

--
-- Whether a folder is a place in the sidebar, and the button that takes it
-- out. The place goes to the Trash as the sidebar's Unpin sends it; the
-- folder is never touched.
--
local pinned = folder and placelib.find(placelib.read(fs), one) or nil

local unpin = ui.button{ text = "Unpin" }

--
-- **What opens its type, and the choice of it** (`roadmap.md` 6z): where
-- more than one application opens a file's type, this row is where the
-- person chooses - for every file of the type, as Preferences' File types
-- chooses, since it is one setting. A dropdown, the default first.
--
local kind = one and not folder and attrs.kind ~= "launcher"
             and types.kind_of(one, attrs) or nil
local able = kind and types.openers(kind) or {}
local chooser = nil

if #able > 1 then
  local choices = {}

  for _, program in ipairs(able) do
    choices[#choices + 1] = { program, types.app_name(program) }
  end

  -- Said once, for the display harness: what it opens with, of what.
  print(("info: .%s opens with %s, of %s"):format(kind,
        tostring(types.opener(one, attrs)), table.concat(able, ", ")))

  chooser = ui.dropdown{ choices = choices, value = types.opener(one, attrs),
                         on_change = function(_, program)
                           local ok, why = types.choose(kind, program)

                           print(("info: .%s opens with %s%s"):format(
                                 kind, tostring(program),
                                 ok and "" or (" - not kept: " .. tostring(why))))
                         end }
end

--
-- The rows, as label, value and a note under the value. Asked again every
-- pass while counting, because the values climb; the rows themselves are
-- the same from the first pass, so nothing moves while it counts.
--
local function rows()
  local out = { { "Where", where_of() } }

  if count then
    local so_far = count.done and "" or " so far"
    local note = grouped(count.bytes) .. " bytes"

    if count.unreadable > 0 then
      note = note .. ", " .. plural(count.unreadable, "folder")
             .. " would not list"
    end

    out[#out + 1] = { "Size", files.size(count.bytes) .. so_far, note }
    out[#out + 1] = { "Contains", plural(count.files, "file") .. " in "
                                  .. plural(count.folders, "folder") }
  else
    local size = tonumber(attrs.size) or 0

    out[#out + 1] = { "Size", files.size(size), grouped(size) .. " bytes" }
  end

  if one and math.type(attrs.modified) == "integer" then
    out[#out + 1] = { "Modified",
                      clock.long_string(clock.at(attrs.modified)) }
  end

  if pinned then
    out[#out + 1] = pinned.gone
                    and { "Sidebar", "Unpinned - the place is in the Trash" }
                    or { "Sidebar", "Pinned", nil, unpin }
  end

  if one and attrs.kind == "launcher" then
    out[#out + 1] = { "Starts", tostring(attrs.program or "?")
                      .. ((attrs.args and attrs.args ~= "")
                          and (" " .. attrs.args) or "") }
  elseif chooser then
    -- What it applies to, dim, where a value would be; the choice itself is
    -- the dropdown at the right.
    out[#out + 1] = { "Opens with", "for every ." .. kind, nil, chooser,
                      dim = true }
  elseif one and not folder then
    local opener = types.opener(one, attrs)

    out[#out + 1] = { "Opens with", opener and types.app_name(opener)
                                    or "Nothing opens it" }
  end

  return out
end

local function row_h(r)
  local h = ROW_PAD * 2 + gfx.height("text") + (r[3] and gfx.height() or 0)

  return math.max(ROW_MIN, h)
end

local card_top = PAD + PICTURE + BELOW
local card_w = W - PAD * 2

local function card_h()
  local h = 0

  for _, r in ipairs(rows()) do h = h + row_h(r) end

  return h
end

-- The row count never changes after the window opens - an Unpin leaves the
-- row saying so rather than taking it out - so the height is fixed here.
local H = card_top + card_h() + PAD + 2

local win, err = ui.window{ title = name, w = W, h = H, x = 260, y = 160 }

if not win then
  print("info: " .. tostring(err))
  return
end

local body = ui.view{ x = 0, y = 0, w = W, h = H,
                      follow = { "left", "right", "top", "bottom" } }

local entry = one and { name = name, kind = attrs.kind, attrs = attrs,
                        full = one == files.TRASH
                               and #(fs.list(files.TRASH) or {}) > 0 }
              or { name = name, kind = "file" }

function body:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  -- The picture, the name and what it is.
  files.icon(g, PAD, PAD, entry, one, PICTURE)

  local tx = PAD + PICTURE + GAP
  local nh, kh = gfx.height("heading"), gfx.height()
  local ty = PAD + (PICTURE - nh - 2 - kh) // 2

  g:text(tx, ty, ui.fitted(name, W - tx - PAD, "heading"), theme.text,
         theme.window, "heading")
  g:text(tx, ty + nh + 2,
         ui.fitted(kind_line(), W - tx - PAD),
         theme.text_dim, theme.window)

  -- The card, and a row at a time inside it.
  local list = rows()
  local ch = 0

  for _, r in ipairs(list) do ch = ch + row_h(r) end

  g:fill_round(PAD, card_top, card_w, ch, theme.sunken, ui.layout.card_r)
  g:frame_round(PAD, card_top, card_w, ch, theme.line_soft, ui.layout.card_r)

  local y = card_top
  local lx = PAD + ROW_IN
  local vx = lx + LABEL_W + LABEL_GAP
  local right = PAD + card_w - ROW_IN

  for i, r in ipairs(list) do
    local h = row_h(r)
    local lines = gfx.height("text") + (r[3] and gfx.height() or 0)
    local top = y + (h - lines) // 2
    local room = right - vx - (r[4] and (r[4].w + LABEL_GAP) or 0)

    if i > 1 then g:fill(PAD + 1, y, card_w - 2, 1, theme.line_soft) end

    g:text(lx, top + (gfx.height("text") - gfx.height()) // 2,
           ui.fitted(r[1], LABEL_W), theme.text_dim, theme.sunken)
    if r.dim then
      g:text(vx, top + (gfx.height("text") - gfx.height()) // 2,
             ui.fitted(r[2], room), theme.text_dim, theme.sunken)
    else
      g:text(vx, top, ui.fitted(r[2], room, "text"), theme.text,
             theme.sunken, "text")
    end

    if r[3] then
      g:text(vx, top + gfx.height("text"), ui.fitted(r[3], room),
             theme.text_dim, theme.sunken)
    end

    y = y + h
  end
end

unpin.on_click = function()
  if not pinned then return end

  local name_, why = files.free_name(files.TRASH, pinned.name)
  local ok = false

  if name_ then
    ok, why = files.move(pinned.file, files.join(files.TRASH, name_))
  end

  if ok then
    pinned.gone = true
    unpin.hidden = true
  else
    print("info: could not unpin " .. tostring(one) .. ": " .. tostring(why))
  end
end

--
-- The Unpin button, or the choice of what opens it, against the card's
-- right, in the middle of its row - placed once, since the rows are the same
-- from the first pass to the last.
--
if pinned or chooser then
  local y = card_top

  for _, r in ipairs(rows()) do
    local h = row_h(r)

    if r[4] == unpin or (r[4] and r[4] == chooser) then
      r[4].x = PAD + card_w - ROW_IN - r[4].w
      r[4].y = y + (h - r[4].h) // 2
    end

    y = y + h
  end
end

--
-- A slice of the walk every pass, and a pass as soon as the last one is
-- done while there is walking left: a window that waited its quarter of a
-- second between slices would take minutes over a drive.
--
local reported = false

function win:on_frame()
  if not count or count.done then
    if count and not reported then
      reported = true
      -- For the display harness, which makes a folder of a known size and
      -- holds this to it. Joined so the typed line does not match itself.
      print(("info" .. ": %s, %d files in %d folders, %d bytes"):format(
            one or name, count.files, count.folders, count.bytes))
    end

    self.poll_wait_ticks = nil
    return false
  end

  count:step(96)

  if count.done then self.poll_wait_ticks = nil end

  return true
end

if count and not count.done then win.poll_wait_ticks = 1 end

win:add(body)
if pinned then win:add(unpin) end
if chooser then win:add(chooser) end
win:run()
