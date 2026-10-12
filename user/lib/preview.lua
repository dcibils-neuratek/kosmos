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
-- it into a surface - the same way Photo shows one. PDF pages, songs and
-- films come in the next steps (Q2-Q4) through the PDF Kit, the audio kit
-- and `video.lua`, the readers Reader, Music and Video already use. Anything
-- it cannot show yet is its icon, large, and what is known about it.

local ui       = use("/Kosmos/Libraries/ui.lua")
local files    = use("/Kosmos/Libraries/files.lua")
local types    = use("/Kosmos/Libraries/filetypes.lua")
local pictures = use("/Kosmos/Libraries/pictures.lua")
local clock    = use("/Kosmos/Libraries/clock.lua")

local theme = ui.theme

local preview = {}

-- What a file shows as: "picture" when a decoder reads it, "pdf" for a
-- PDF, else "facts".
function preview.kind(path, attrs)
  if attrs and attrs.kind and attrs.kind ~= "file" then return "facts" end
  if pictures.decoder(path) then return "picture" end
  if types.kind_of(path, attrs) == "pdf" then return "pdf" end
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

  -- The PDF given back: its pages unlent and freed, its file closed.
  local function let_go()
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
