-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A file, shown: the view Quick Look puts in its panel.
--
--   local preview = use("/Kosmos/Libraries/preview.lua")
--   local p = preview.new{ x = 0, y = 0, w = 600, h = 400 }
--   p:show("/Home/Photos/harbour.jpg", attrs)      -- the entry's attributes
--   p.facts        "JPEG picture · 2.4 MB · 4032 x 3024"
--   p.opener       "Photo", the application that opens it, or nil
--
-- **A library, not Tracker's own code** (`docs/quicklook.html`, agreed 11
-- October 2026; Diego: "That's why we did the kits - we can reuse all of
-- them for this"). Tracker's Quick Look is the first to show a file in a
-- panel; the Open window's preview and Mail's attachments want exactly this
-- next, so it is written once.
--
-- **It decodes nothing.** A picture is the UI kit's `ui.image`, which names
-- the file and has `pictures.lua` read it into pages and the C decoders turn
-- it into a surface - the same way Photo shows one. A PDF's pages are the
-- PDF Kit's (Q2), as the PDF viewer draws them; a song is `media.lua`'s
-- (Q3), as Music plays it, with its cover and names from the tag reader.
-- Films come next (Q4) through `video.lua`. Anything it cannot show is its
-- icon, large, and what is known about it.
--
-- **A song plays as soon as it is shown**, and stops when another file is
-- or the view is released. It needs feeding: whoever holds the view calls
-- `p:tick()` often while `p:sounding()` - Tracker from a ticker of its own,
-- since the view sits inside a sheet.

local ui       = use("/Kosmos/Libraries/ui.lua")
local files    = use("/Kosmos/Libraries/files.lua")
local types    = use("/Kosmos/Libraries/filetypes.lua")
local pictures = use("/Kosmos/Libraries/pictures.lua")
local clock    = use("/Kosmos/Libraries/clock.lua")

local theme = ui.theme

local preview = {}

-- What `media.lua` plays (Q3): the songs it has decoders for.
local SONGS = { mp3 = true, wav = true }
local media = nil

-- What a file shows as: "picture" when a decoder reads it, "pdf" for a
-- PDF, else "facts".
function preview.kind(path, attrs)
  if attrs and attrs.kind and attrs.kind ~= "file" then return "facts" end
  if pictures.decoder(path) then return "picture" end
  if types.kind_of(path, attrs) == "pdf" then return "pdf" end
  if SONGS[(tostring(path):lower():match("%.(%w+)$") or "")] then return "song" end
  return "facts"
end

--
-- **A PDF's pages** (Q2): the PDF Kit opens it and draws a page, as the
-- PDF viewer does, into a surface of the page's size at the panel's width -
-- in C, once - and the surface is lent to the painter by a name
-- (`ui.lend_picture`), so the kit window draws it with no copy. Pages are
-- drawn as they come into view and the four nearest kept; the rest are
-- given back. Every page is taken at the first one's size, which is nearly
-- always true of a document and only ever a little wrong when it is not.
--
local PAPER, INK = 0xfff4f1ea, 0xff101418     -- the PDF viewer's
local GAP, KEEP = 14, 4
local pdf = nil
local lends = 0

local function name_of(path)
  return tostring(path):match("([^/]+)$") or tostring(path)
end

local function folder_of(path)
  local dir = tostring(path):match("^(.*)/[^/]+$")
  return (dir == nil or dir == "") and "/" or dir
end

-- When it was last changed, in words, or nil when the disk does not say.
local function modified(attrs)
  local m = attrs and attrs.modified

  if math.type(m) ~= "integer" or m <= 0 then return nil end
  return clock.long_string(clock.at(m, clock.offset()))
end

function preview.new(spec)
  local v = ui.view(spec)

  v.path, v.attrs, v.facts, v.opener = nil, nil, "", nil

  lends = lends + 1
  local named = "preview" .. lends .. ":page"
  local doc = nil               -- the open PDF: { pdf, n, w, h, scale, made, top }
  local song = nil              -- the song: { player, tags, cover, seconds }

  -- The PDF given back - its pages unlent and freed, its file closed - and
  -- the song stopped.
  local function let_go()
    if song then
      song.player:close()
      song = nil
    end

    if not doc then return end

    for i, surface in pairs(doc.made) do
      ui.lend_picture(named .. i, nil)
      surface:free()
    end

    if doc.pdf.source and doc.pdf.source.close then doc.pdf.source.close() end
    doc = nil
  end

  v.release = let_go

  local function open_pdf(path)
    pdf = pdf or use("/Kosmos/Libraries/pdf.lua")

    local ok, opened = pcall(pdf.open, path)

    if not ok or not opened or #opened.pages == 0 then
      return nil, ok and "no pages" or tostring(opened)
    end

    local box = opened:resolve(opened:page(1).MediaBox) or { 0, 0, 612, 792 }
    local pw = (box[3] or 612) - (box[1] or 0)
    local ph = (box[4] or 792) - (box[2] or 0)
    local w = math.max(120, math.min(v.w - 48, 760))
    local scale = w / pw

    return { pdf = opened, n = #opened.pages, w = w, h = math.floor(ph * scale + 0.5),
             scale = scale, made = {}, top = 0 }
  end

  -- Page `i`, drawn if it is not, the farthest of the rest given back.
  local function page(i)
    if doc.made[i] then return true end

    local ok, surface = pcall(gfx.surface, { w = doc.w, h = doc.h })

    if not ok or not surface then return false end

    surface:fill(0, 0, doc.w, doc.h, PAPER)
    local drawn, why = pcall(pdf.render, doc.pdf, doc.pdf:page(i), surface, doc.scale, INK)

    if not drawn then print(("preview: page %d of %s not drawn: %s"):format(i, tostring(v.path), tostring(why))) end
    doc.made[i] = surface
    ui.lend_picture(named .. i, surface)

    local count, far, far_d = 0, nil, -1

    for j in pairs(doc.made) do
      count = count + 1
      if math.abs(j - i) > far_d then far, far_d = j, math.abs(j - i) end
    end

    if count > KEEP and far then
      ui.lend_picture(named .. far, nil)
      doc.made[far]:free()
      doc.made[far] = nil
    end

    return true
  end

  -- Where the page at the top of the view is, for the number in the corner.
  function v:page_shown()
    if not doc then return nil end
    return math.min(doc.n, doc.top // (doc.h + GAP) + 1), doc.n
  end

  function v:wheel(n)
    if not doc then return false end

    local most = math.max(0, doc.n * (doc.h + GAP) + GAP - self.h)

    doc.top = math.max(0, math.min(most, doc.top - n * 60))

    if self.on_page then self.on_page(self:page_shown()) end
    return true
  end

  --
  -- **A song** (Q3): opened, its names and cover read, and playing.
  --
  local function open_song(path)
    media = media or use("/Kosmos/Libraries/media.lua")

    local player, why = media.open(path)

    if not player then return nil, why end

    local tags = media.tags(path) or {}

    player:play()

    return { player = player, tags = tags, cover = media.cover(path),
             seconds = player.info.seconds or 0 }
  end

  -- Seconds heard of the song shown, or nil.
  function v:song_position()
    return song and song.player:position() or nil
  end

  -- Whether there is a song to feed, and feeding it: true while it plays.
  function v:sounding()
    return song ~= nil and song.player.playing
  end

  function v:tick()
    if not song then return false end

    local p = song.player

    p:tick()

    if p.error then
      print(("preview: %s stopped: %s"):format(tostring(self.path), tostring(p.error)))
      p.playing = false
    elseif p:finished() then
      p:pause()
      p:seek(0)
    end

    return p.playing
  end

  -- Where things are in a song's view, for the drawing and the pointer.
  local function song_layout(w, h)
    local cover = math.max(96, math.min(220, h - 80, (w - 80) // 2))
    local x = math.max(24, (w - cover - 26 - math.min(360, w - cover - 90)) // 2)
    local tx = x + cover + 26
    local tw = math.max(120, math.min(360, w - tx - 24))
    local y = (h - cover) // 2

    return { cover = cover, x = x, y = y, tx = tx, tw = tw,
             button = { x = tx, y = y + cover // 2 + 6, d = 34 },
             bar = { x = tx + 34 + 12 + 40, y = y + cover // 2 + 6 + 17,
                     w = math.max(40, tw - 34 - 12 - 80) } }
  end

  local function clock_of(seconds)
    seconds = math.max(0, math.floor(seconds or 0))
    return ("%d:%02d"):format(seconds // 60, seconds % 60)
  end

  -- The play button pauses and plays; the bar takes you where you press.
  function v:mouse(action, x, y)
    if action ~= "press" or self.kind ~= "song" or not song then return false end

    local L = song_layout(self.w, self.h)
    local b, bar = L.button, L.bar

    if x >= b.x and x < b.x + b.d and y >= b.y and y < b.y + b.d then
      if song.player.playing then song.player:pause() else song.player:play() end
      print(("preview: %s %s"):format(song.player.playing and "playing" or "paused",
                                      tostring(self.path)))
      if self.on_sound then self.on_sound(song.player.playing) end
      return true
    end

    if song.seconds > 0 and x >= bar.x and x < bar.x + bar.w
       and math.abs(y - bar.y) <= 8 then
      song.player:seek(song.seconds * (x - bar.x) / bar.w)
      return true
    end

    return false
  end

  -- The picture, laid over this view when it shows one.
  local picture = ui.image{ x = 0, y = 0, w = v.w, h = v.h, contain = true, centre = true,
                            ground = theme.window, follow = { "left", "right", "top", "bottom" } }

  picture.hidden = true
  v:add(picture)

  --
  -- Which file, changed while the panel is open: the arrows move through a
  -- folder and the panel follows. True when it was shown as itself, false
  -- when as its icon and its facts.
  --
  function v:show(path, attrs)
    attrs = attrs or (fs.getattr(path) or {})
    let_go()
    self.path, self.attrs = path, attrs

    local opener = types.opener(path, attrs)

    self.opener = opener and types.app_name(opener) or nil
    self.kind = preview.kind(path, attrs)

    local said = { types.describe(path, attrs) }

    if attrs.kind ~= "directory" and attrs.size then
      said[#said + 1] = files.size(attrs.size)
    end

    if self.kind == "pdf" then
      picture.hidden = true

      local why

      doc, why = open_pdf(path)

      if doc then
        said[#said + 1] = doc.n == 1 and "1 page" or (doc.n .. " pages")
      else
        self.kind = "facts"
        said[#said + 1] = "it would not open: " .. tostring(why)
      end
    elseif self.kind == "song" then
      picture.hidden = true

      local why

      song, why = open_song(path)

      if song then
        said[#said + 1] = clock_of(song.seconds)
      else
        self.kind = "facts"
        said[#said + 1] = "it would not play: " .. tostring(why)
      end
    elseif self.kind == "picture" then
      picture.w, picture.h = self.w, self.h
      picture.hidden = not picture:set(path)

      if picture.hidden then
        self.kind = "facts"
      else
        said[#said + 1] = ("%d x %d"):format(picture.image_w, picture.image_h)
      end
    else
      picture.hidden = true
    end

    self.facts = table.concat(said, " · ")
    if self.on_sound then self.on_sound(self:sounding()) end
    return self.kind ~= "facts"
  end

  -- Anything it cannot show: the icon, large, and what is known.
  function v:draw(g)
    g:fill(0, 0, self.w, self.h, theme.window)

    -- The pages one under another, as many as are in view.
    if self.kind == "pdf" and doc then
      local x = (self.w - doc.w) // 2
      local first = doc.top // (doc.h + GAP) + 1

      for i = first, doc.n do
        local y = GAP + (i - 1) * (doc.h + GAP) - doc.top

        if y >= self.h then break end

        if page(i) then
          g:lent(x, y, doc.w, doc.h, named .. i, doc.w, doc.h)
        end
      end

      local at, of = self:page_shown()
      local badge = ("%d / %d"):format(at, of)
      local bw = gfx.measure(badge) + 16

      g:fill_round(self.w - bw - 12, 10, bw, gfx.font.h + 6, theme.raised, 9)
      g:text(self.w - bw - 4, 13, badge, theme.text)
      return
    end

    -- A song: its cover, its names, the button and where it has got to.
    if self.kind == "song" and song then
      local L = song_layout(self.w, self.h)
      local tags = song.tags

      if song.cover then
        g:picture(L.x, L.y, L.cover, L.cover, song.cover)
      else
        g:fill_round(L.x, L.y, L.cover, L.cover, theme.raised, 8)
        files.icon(g, L.x + (L.cover - 96) // 2, L.y + (L.cover - 96) // 2,
                   { name = name_of(self.path), kind = "file", attrs = self.attrs or {} },
                   self.path, 96)
      end

      local title = tags.title or name_of(self.path)
      local who = tags.artist or "Unknown artist"

      if tags.album then who = who .. " \u{b7} " .. tags.album end

      g:text(L.tx, L.y + L.cover // 2 - 2 * gfx.font.h - 8, ui.fitted(title, L.tw),
             theme.text, nil, "heading")
      g:text(L.tx, L.y + L.cover // 2 - gfx.font.h - 6, ui.fitted(who, L.tw), theme.text_dim)

      local b, bar = L.button, L.bar
      local r = b.d // 2

      g:fill_round(b.x, b.y, b.d, b.d, theme.text, r)

      if song.player.playing then
        g:fill(b.x + 11, b.y + 10, 4, 14, theme.window)
        g:fill(b.x + 19, b.y + 10, 4, 14, theme.window)
      else
        g:triangle(b.x + 13, b.y + 9, b.x + 13, b.y + 25, b.x + 25, b.y + 17, theme.window)
      end

      local at = math.min(song.seconds, song.player:position())
      local done = song.seconds > 0 and (bar.w * at // song.seconds) or 0

      g:text(bar.x - 40, bar.y - gfx.font.h // 2, clock_of(at), theme.text_dim)
      g:fill_round(bar.x, bar.y - 2, bar.w, 4, theme.line_soft, 2)
      g:fill_round(bar.x, bar.y - 2, math.max(4, done), 4, theme.text, 2)
      g:text(bar.x + bar.w + 8, bar.y - gfx.font.h // 2, clock_of(song.seconds), theme.text_dim)
      return
    end

    if self.kind ~= "facts" or not self.path then return end

    local size = 128
    local x = (self.w - size) // 2
    local y = math.max(12, self.h // 2 - size - 10)
    local attrs = self.attrs or {}

    files.icon(g, x, y, { name = name_of(self.path), kind = attrs.kind, attrs = attrs },
               self.path, size)

    local rows = { { "Kind", types.describe(self.path, attrs) } }

    if attrs.kind ~= "directory" and attrs.size then
      rows[#rows + 1] = { "Size", files.size(attrs.size) }
    end

    if modified(attrs) then rows[#rows + 1] = { "Modified", modified(attrs) } end
    rows[#rows + 1] = { "Where", folder_of(self.path) }

    local ty = y + size + 18
    local mid = self.w // 2

    for _, r in ipairs(rows) do
      g:text(mid - 8 - gfx.measure(r[1]), ty, r[1], theme.text_dim)
      g:text(mid + 8, ty, ui.fitted(r[2], self.w - mid - 24), theme.text)
      ty = ty + gfx.font.h + 6
    end
  end

  return v
end

return preview
