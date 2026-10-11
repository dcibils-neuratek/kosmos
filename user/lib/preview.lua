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

-- What a file shows as: "picture" when a decoder reads it, else "facts".
function preview.kind(path, attrs)
  if attrs and attrs.kind and attrs.kind ~= "file" then return "facts" end
  if pictures.decoder(path) then return "picture" end
  return "facts"
end

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
    self.path, self.attrs = path, attrs

    local opener = types.opener(path, attrs)

    self.opener = opener and types.app_name(opener) or nil
    self.kind = preview.kind(path, attrs)

    local said = { types.describe(path, attrs) }

    if attrs.kind ~= "directory" and attrs.size then
      said[#said + 1] = files.size(attrs.size)
    end

    if self.kind == "picture" then
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
