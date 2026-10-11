-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- A picture by name, from wherever the name says.
--
--   local pictures = use("/Kosmos/Libraries/pictures.lua")
--   local s, why = pictures.from("/Home/Captures/screenshot.png")   -- a file
--   local s, why = pictures.from("64x64/App_Mail.png")              -- an asset
--   local keep = pictures.kept(4)      -- files decoded once, four at most
--   local s = keep:get(name)
--
-- **A path beginning `/` is a file; any other name is carried in the image**
-- and handed over by `sys.asset` - the icons, the test pictures, the
-- desktop's wallpapers. The file is read into pages (`regions.lua`) and
-- decoded from there, so its bytes never become a Lua string: a photograph
-- is megabytes, and the decoder is C.
--
-- **One door.** This was the window manager's, from when it drew every
-- window's pictures. Windows paint themselves now (`docs/astra-display.md`
-- D2c), and the kit's own lookup knew only the image's assets - so a file
-- named to `ui.image` drew nothing, and Photo showed every capture black
-- (Diego, on the M700, 10 October 2026: "printi screen saves a black image").
-- The window manager and the kit both come here now.
--

local regions = use("/Kosmos/Libraries/regions.lua")

local pictures = {}

local DECODERS = {
  png  = function(...) return gfx.png(...) end,
  jpg  = function(...) return gfx.jpeg(...) end,
  jpeg = function(...) return gfx.jpeg(...) end,
}

-- The decoder a name's suffix asks for, and the suffix.
function pictures.decoder(name)
  local suffix = tostring(name):lower():match("%.([%a]+)$")

  return suffix and DECODERS[suffix], suffix
end

-- A surface, or nil and why.
function pictures.from(name)
  local decode, suffix = pictures.decoder(name)

  if not decode then
    return nil, "this opens PNG and JPEG, and that is neither"
  end

  if name:sub(1, 1) ~= "/" then
    local bytes, why = sys.asset(name)

    if not bytes then return nil, why end

    local ok, made = pcall(decode, bytes)

    if not ok or not made then
      return nil, tostring(made or ("that " .. suffix .. " would not decode"))
    end

    return made
  end

  local size = (fs.getattr(name) or {}).size

  if not size or size == 0 then
    return nil, "cannot read " .. tostring(name)
  end

  local region = regions.make(size)

  if not region then
    return nil, "no memory for a picture that size"
  end

  local done = regions.read_file(name, region, size) or 0

  if done ~= size then
    regions.free(region)
    return nil, "could only read " .. done .. " of " .. size .. " bytes"
  end

  local ok, made = pcall(decode, region.at, size)

  -- The compressed copy is scratch: the surface holds the pixels now.
  regions.free(region)

  if not ok or not made then
    -- `made` is the decoder's own sentence when the call raised, which is
    -- the one that says *what* was wrong with the file.
    return nil, tostring(made or ("that " .. suffix .. " would not decode"))
  end

  return made
end

--
-- **Kept**: each name decoded once, and a wrong one remembered as wrong so
-- it is not read again on every paint. An asset is kept for good; a file is
-- one of the last `bound` (four, as the window manager has always kept), and
-- the oldest is freed when another comes - a photograph is ten megabytes
-- decoded, and a viewer stepping through a folder would otherwise keep them
-- all.
--
function pictures.kept(bound)
  local cache, files = {}, {}
  local keep = {}

  bound = bound or 4

  function keep:get(name)
    local got = cache[name]

    if got ~= nil then return got or nil end

    local made = pictures.from(name)

    cache[name] = made or false

    if made and name:sub(1, 1) == "/" then
      files[#files + 1] = name

      while #files > bound do
        local old = table.remove(files, 1)
        local gone = cache[old]

        cache[old] = nil
        if gone and gone ~= made then gone:free() end
      end
    end

    return made
  end

  return keep
end

return pictures
