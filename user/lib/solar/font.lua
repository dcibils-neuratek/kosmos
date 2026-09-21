-- solar/font.lua — bitmap font loader. Sizes live in solar/fonts/f<px>.lua and are
-- required on demand, then decoded once into per-glyph arrays of alpha 0..1.

local F = { sizes = { 12, 24, 36, 48, 72 } }
local cache = {}

function F.get(px)
  local font = cache[px]
  if font then return font end
  font = require("solar.fonts.f" .. px)
  font.glyphs = {}
  for i, s in ipairs(font.data) do
    local g = {}
    for k = 1, #s do g[k] = tonumber(s:sub(k, k), 16) / 15 end
    font.glyphs[font.first + i - 1] = g
  end
  font.data = nil
  cache[px] = font
  return font
end

return F
