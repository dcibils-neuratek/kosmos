-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Which face a look is set in, and its measure in points (`docs/write.md`,
-- W2).
--
--   local faces = use("/Kosmos/Libraries/faces.lua")
--   local catalogue = faces.catalogue(gfx.typefaces())
--   local measure = faces.measure(catalogue, gfx.typeface)
--   pageset.set(doc, measure)
--
-- A look names a face as a person does - `IBM Plex Serif`, `SemiBold`,
-- italic - and the image carries files. **The catalogue is what the fonts
-- say they are** (`gfx.typefaces()`: each one's family, weight class and italic
-- bit, read from the font), and `pick` finds a look's face in it: the
-- family it names, the same slant if there is one, and the nearest weight.
-- A family this machine does not have is set in `faces.FALLBACK`, and `pick`
-- says so, so a document from another machine opens and keeps its own face
-- names for the day it goes back.
--
-- **The measure is the font's own advance widths, unhinted**, scaled by the
-- size in points - the widths a PDF carries - so a page set here is the page
-- the PDF prints (`face.c` has the argument).

local faces = {}

-- A weight's name, as a style says it, and the class a font says.
faces.WEIGHT = { Light = 300, Regular = 400, Medium = 500, SemiBold = 600,
                 Bold = 700 }

-- What a family this machine does not have is set in.
faces.FALLBACK = "IBM Plex Sans"

--
-- A catalogue from a list of `{ file, family, weight, italic }`: the
-- families, each with its faces, and their names in order for a menu.
--
function faces.catalogue(list)
  local families, names = {}, {}

  for _, f in ipairs(list or {}) do
    if type(f.family) == "string" and type(f.file) == "string" then
      if not families[f.family] then
        families[f.family] = {}
        names[#names + 1] = f.family
      end

      local into = families[f.family]
      into[#into + 1] = { file = f.file, family = f.family,
                          weight = tonumber(f.weight) or 400,
                          italic = f.italic == true }
    end
  end

  table.sort(names)

  return { families = families, names = names }
end

--
-- **The face a look is set in**, and whether it is the one the look asked
-- for. The slant first - an upright face is never chosen over an italic one
-- that exists - then the weight nearest the one asked, the heavier of two
-- equally near. Nil when the catalogue has nothing at all.
--
function faces.pick(catalogue, look)
  local family = catalogue.families[look.face]
  local exact = family ~= nil

  if not family then
    family = catalogue.families[faces.FALLBACK]
             or catalogue.families[catalogue.names[1]]
  end

  if not family then return nil, false end

  local want = faces.WEIGHT[look.weight] or 400
  local italic = look.italic == true
  local best, best_score

  for _, f in ipairs(family) do
    -- The slant decides before the weight does: a wrong slant costs more
    -- than the whole range of weights.
    local score = math.abs(f.weight - want) * 2 - (f.weight >= want and 1 or 0)
                  + (f.italic == italic and 0 or 10000)

    if not best or score < best_score then
      best, best_score = f, score
    end
  end

  return best, exact and best.italic == italic and best.weight == want
end

--
-- **A measure for `pageset`**, over the catalogue and `open`, which makes a
-- face from a file (`gfx.typeface` inside the machine). Each file is opened
-- once and each look's face picked once.
--
function faces.measure(catalogue, open)
  local opened, picked = {}, {}

  local function face_of(look)
    local key = look.face .. "\0" .. tostring(look.weight) .. "\0"
                .. tostring(look.italic)
    local face = picked[key]

    if not face then
      local f = faces.pick(catalogue, look)

      if not f then error("faces: no face to set text in", 2) end

      face = opened[f.file]

      if not face then
        local made, why = open(f.file)

        if not made then error("faces: " .. tostring(why), 2) end

        local units, ascent, descent, gap = made:metrics()
        face = { face = made, file = f.file, units = units, ascent = ascent,
                 descent = descent, gap = gap }
        opened[f.file] = face
      end

      picked[key] = face
    end

    return face
  end

  return {
    -- `ligatures`: f and what follows as one glyph where the face has one,
    -- as the document's switch says (`face.c`).
    width = function(look, text, ligatures)
      local f = face_of(look)
      return f.face:advance(text, ligatures) * look.size_pt / f.units
    end,

    -- Ascent and descent as distances, both positive; the gap below.
    line = function(look)
      local f = face_of(look)
      local scale = look.size_pt / f.units
      return f.ascent * scale, -f.descent * scale, f.gap * scale
    end,

    face_of = face_of,
  }
end

return faces
