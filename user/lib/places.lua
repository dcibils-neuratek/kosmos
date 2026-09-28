-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Places: shortcuts a person makes and names, found again by what they are.
--
-- `drives.html` draws `MyPhotos on PHOTOS 2024` beside Home in Tracker's
-- sidebar: a drive, or a folder on one, dragged into Places and given a name.
-- **A shortcut, not a mount** - the drive itself never changes, and an Open
-- window hands an application the file at its real place, so no program
-- has to know MyPhotos exists.
--
-- Each place is a file in `/Home/Places`, named what the person called it,
-- with attributes that say what it points at in words anybody can read:
--
--   kind = "place", path = "/Home/Music"                  somewhere fixed
--   kind = "place", volume = "fat:1A2B-3C4D",             on a drive
--     within = "/Italy", volume_name = "PHOTOS 2024"
--
-- **A place on a drive keys on the volume's own identity, never its name or
-- its unit.** A name depends on the order drives arrived, so `PHOTOS` can
-- come back as `PHOTOS 2`; a unit is handed out afresh on every replug
-- (`units_named++` in `xhci.c`). `/Drives` reports what the volume *is* - a
-- FAT serial or a GPT partition's GUID (`usb.md` 6c) - and that travels with
-- it. `volume_name` is only what it was called last, for the note shown
-- while it is away; nothing is ever found by it.
--
-- Here rather than in `tracker.lua` for `deskbarmenu.lua`'s reason: this is
-- the part with the decisions in it, and a decision is worth testing on the
-- build machine, where a test costs no boot. `store` is anything answering
-- `list(path)` and `getattr(path)` - `fs` in Tracker, a table in
-- `tools/test_places.lua`.

local places = {}

places.DIR = "/Home/Places"

--
-- **The places a person keeps** (`roadmap.md` 6w), after Home and Desktop in
-- Tracker's sidebar and before anything pinned: a folder each in `/Home`,
-- made the first time it is missing, as Tracker makes `Desktop`. Diego, 27
-- September: "tracker needs places like photos, documents, movies,
-- captures, music", and "Captures is basically webcam video captures". The
-- applications keep what they make and look first in them, each naming its
-- own: the Camera's recordings in `/Home/Captures`, Music and Video starting
-- in `/Home/Music` and `/Home/Movies`, Cafesa3D's scenes and renders in
-- `/Home/Documents`.
--
places.STANDARD = {
  { name = "Documents", path = "/Home/Documents", icon = "document" },
  { name = "Photos",    path = "/Home/Photos",    icon = "pictures" },
  { name = "Movies",    path = "/Home/Movies",    icon = "movies" },
  { name = "Captures",  path = "/Home/Captures",  icon = "captures" },
  { name = "Music",     path = "/Home/Music",     icon = "music" },
}

--
-- A path under `/Drives`, as its volume's name and the rest - `/` for the
-- volume itself. Anything else is not on a drive.
--
local function on_drive(path)
  local name, rest = path:match("^/Drives/([^/]+)(.*)$")

  if not name then return nil end

  rest = rest:gsub("/+$", "")

  return name, (rest == "") and "/" or rest
end

local function tidy(path)
  path = tostring(path or "")

  if #path > 1 then path = path:gsub("/+$", "") end

  return path
end

--
-- What to store for a place made from `path`, given what is plugged in now.
-- The attributes, or nil and a sentence saying why not.
--
function places.from_path(path, volumes)
  path = tidy(path)

  if path == "" then return nil, "nothing to make a place of" end

  local name, within = on_drive(path)

  if not name then
    return { kind = "place", path = path }
  end

  for _, v in ipairs(volumes or {}) do
    if v.name == name then
      --
      -- **Refused rather than remembered by its name.** A volume on an MBR
      -- drive whose filesystem carries no serial has nothing that would still
      -- be true of it after a replug, and a shortcut that quietly opened
      -- whichever stick next arrived under that name would be worse than none.
      --
      if not v.id then
        return nil, name .. " has nothing to know it by once it is unplugged"
      end

      return { kind = "place", volume = v.id, within = within,
               volume_name = v.name }
    end
  end

  return nil, name .. " is not plugged in"
end

--
-- Where a stored place is now: its path and, for one on a drive, what that
-- volume is called today. Or nil and why.
--
-- **Found by identity alone.** Another stick that happens to be called
-- `PHOTOS` is not this place's volume, and saying "unplugged" is the right
-- answer while the real one is away.
--
function places.resolve(attrs, volumes)
  attrs = attrs or {}

  if attrs.path then return attrs.path end

  if not attrs.volume then return nil, "not a place" end

  for _, v in ipairs(volumes or {}) do
    if v.id == attrs.volume then
      local base = "/Drives/" .. v.name
      local within = attrs.within or "/"

      return (within == "/") and base or (base .. within), v.name
    end
  end

  return nil, "unplugged"
end

--
-- Every place in `dir`, each as { name = , file = , attrs = }. Only
-- `kind == "place"` counts - anything else somebody keeps in the folder is
-- their business, as in the Deskbar's.
--
-- **In the order they were pinned** (`roadmap.md` 6za, Diego's 6: "yes"),
-- which `order` records - a number one past the highest, given when a place
-- is made. A place made before there was an order has none and comes first,
-- by name without regard to case, as every place did.
--
function places.read(store, dir)
  dir = dir or places.DIR

  local out = {}

  for _, name in ipairs(store.list(dir) or {}) do
    local file = dir .. "/" .. name
    local attrs = store.getattr(file) or {}

    if attrs.kind == "place" then
      out[#out + 1] = { name = name, file = file, attrs = attrs }
    end
  end

  table.sort(out, function(a, b)
    local x, y = tonumber(a.attrs.order) or 0, tonumber(b.attrs.order) or 0

    if x ~= y then return x < y end

    return a.name:lower() < b.name:lower()
  end)

  return out
end

-- The `order` a new place is given: one past the highest there is.
function places.next_order(list)
  local top = 0

  for _, p in ipairs(list or {}) do
    top = math.max(top, tonumber(p.attrs.order) or 0)
  end

  return top + 1
end

--
-- **The place that is `path`**, when there is one: what Pin and Unpin ask,
-- and Info's Sidebar line. Found by where it leads now - so a place on a
-- drive is found only while that drive is plugged in, which is the one time
-- anybody can be looking at its folder - and whatever the case either is
-- written in, as every name is found.
--
function places.find(list, path, volumes)
  local want = tidy(path):lower()

  for _, p in ipairs(list or {}) do
    local there = places.resolve(p.attrs, volumes)

    if there and tidy(there):lower() == want then return p end
  end

  return nil
end

--
-- **Where there are no files** (Diego, 27 September 2026, after opening
-- `/Running` hung Tracker: "why do we need access to 'running' in tracker
-- in the first place", "tracker is for files", "not processes"). Three of
-- the root's seven names are for programs: `/Running`, where one finds
-- another; `/Devices`, the machine's devices; `/Network`, a protocol rather
-- than a tree. Tracker and the Open and Save window leave them out of the
-- root and of System. The Terminal, which is for programs, has them all.
--
places.NOT_FILES = { "/Devices", "/Network", "/Running" }

-- Is `path` somewhere files are? Whatever the case it is spelled in, as
-- every name is found.
function places.holds_files(path)
  local key = tostring(path or ""):lower()

  for _, root in ipairs(places.NOT_FILES) do
    local k = root:lower()

    if key == k or key:sub(1, #k + 1) == k .. "/" then return false end
  end

  return true
end

-- A folder's entries as a window for people shows them: all of them, but
-- at the root only the names that hold files.
function places.files_only(path, entries)
  if path ~= "/" then return entries end

  local out = {}

  for _, e in ipairs(entries or {}) do
    if places.holds_files("/" .. tostring(e.name)) then out[#out + 1] = e end
  end

  return out
end

--
-- A name to offer for a new place: the folder's own, or the volume's when it
-- is the volume itself. Offered, not imposed - the field it goes into is
-- there to be typed over.
--
function places.suggest(path)
  path = tidy(path)

  local name, within = on_drive(path)

  if name and within == "/" then return name end

  return path:match("([^/]+)$") or path
end

return places
