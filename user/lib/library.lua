-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Library kit: songs, albums and artists, from the files themselves.
--
--   local library = use("/Kosmos/Libraries/library.lua")
--   local lib = library.open{ folders = { "/Home/Music" } }
--   lib:refresh(on_progress)          -- reads only what is new or changed
--   lib:songs{ sort = "artist", where = rule, from = 1, count = 200 }
--   lib:albums()  lib:album(id)  lib:artists()  lib:artist(name)
--   lib:search("night fe")            -- { songs, albums, artists }
--   lib:smart(rule)                   -- the songs a rule chooses
--   lib:playlists()  lib:playlist(name)
--   lib:played(path)  lib:skipped(path)  lib:rate(path, 4)
--
-- **Facts, never samples** (`docs/player.md`, P4). What a file says about
-- itself is read once, by `tags.lua`, and kept **as attributes on the file**
-- - `audio:title`, `audio:artist`, `audio:rate` and the rest, decision 3 -
-- so Tracker, the Open window's preview and Automations read the same facts
-- with `getattr` and `query` and never link this. The file's bytes are never
-- written: Diego, 14 September 2026, tags are read and not written
-- (decision 4), and a rating or a play is an attribute too.
--
-- **Read again only when the file changed.** Beside the facts sit the
-- file's stamp and size as they were when it was read (`audio:read_mtime`,
-- `audio:read_size`). A refresh walks the folders with `files.entries`,
-- which hands every file's attributes over with its name, and a file whose
-- stamp and size are the ones written beside its facts is not opened at
-- all - so a library of ten thousand songs refreshed with one new album
-- reads one album. Setting an attribute does not move a file's stamp
-- (`kfs.c`, `kfs_write_attrs`), which is what makes the comparison hold.
--
-- **A drive that keeps no attributes** - a FAT stick - has its files read
-- the first time a process sees them and kept in this process's memory
-- only, by stamp and size where the drive gives them; nothing is written to
-- it (decision 3).
--
-- **Independent of any window.** Nothing here draws or waits on a window:
-- a refresh is `start` and then `step` a few files at a time, which is how
-- the player keeps its window moving while it reads, and Video wants the
-- same kit for films with its own `kinds`. `fs` is the global every
-- process has, and the host test hands it one kept in memory
-- (`tools/test_library.lua`), with the tag reader behind `tags_of`.

local library = {}

local files = use("/Kosmos/Libraries/files.lua")

-- The extensions that are songs, unless the caller names its own.
library.MUSIC = { mp3 = true, flac = true, m4a = true, wav = true,
                  aif = true, aiff = true, aifc = true, ogg = true,
                  oga = true, opus = true }

-- Where playlists live, small files of their own (decision 3).
library.PLAYLISTS = "/Home/Music/Playlists"

-- A folder's own picture, when its songs carry none (`player.md`, Albums).
local FOLDER_COVERS = { ["cover.jpg"] = 1, ["folder.jpg"] = 2,
                        ["cover.png"] = 3, ["folder.png"] = 4,
                        ["cover.jpeg"] = 5, ["folder.jpeg"] = 6 }

-- The longest text kept as an attribute, in bytes. A title is words; the
-- disk takes a request's changes in a kilobyte (`DISK_DATA_MAX`) and all of
-- a file's attributes in a block, so a field of four kilobytes would cost
-- the file every other fact.
local TEXT_MOST = 255

-- The most of one `setattr`'s changes, estimated, under the kilobyte a
-- request carries: keys, values, and a few bytes of packing each.
local CHANGES_MOST = 768

local SECONDS_A_DAY = 86400

--------------------------------------------------------------------------
-- The store: which attribute holds which fact.
--------------------------------------------------------------------------

--
-- **What the file says**, read by `tags.lua` (and, for the sound's own
-- format, by what `tags.lua` reads of the stream's header). Cleared and
-- written again whenever the file is read again, so a tag taken out of the
-- file leaves the library too.
--
-- Text, then numbers, then the one flag. `duration_ms` is milliseconds;
-- `rate` is Hz; `bits` the source's depth.
--
local TEXT = { "title", "artist", "album", "album_artist", "composer",
               "genre", "format", "codec", "cover_mime" }
local NUMBER = { "year", "track", "tracks", "disc", "discs", "rate", "bits",
                 "channels", "duration_ms", "cover_offset", "cover_bytes",
                 "gain_track", "peak_track", "gain_album", "peak_album" }
local FLAG = { "compilation" }

--
-- **What the person did**, kept by the library and never by the file:
-- plays, skips, the rating (1 to 5), the last time it was played and the
-- day it arrived, as seconds since 1970. Never cleared by a read.
--
local PERSON = { "plays", "skips", "rating", "played", "added" }

-- The stamp and size the facts were read at.
local READ_MTIME, READ_SIZE = "audio:read_mtime", "audio:read_size"

local function key(field) return "audio:" .. field end

library.ATTRIBUTES = {}

for _, list in ipairs({ TEXT, NUMBER, FLAG, PERSON }) do
  for _, f in ipairs(list) do library.ATTRIBUTES[#library.ATTRIBUTES + 1] = key(f) end
end

library.ATTRIBUTES[#library.ATTRIBUTES + 1] = READ_MTIME
library.ATTRIBUTES[#library.ATTRIBUTES + 1] = READ_SIZE

--------------------------------------------------------------------------
-- Small things.
--------------------------------------------------------------------------

-- A text cut to `most` bytes at a character, never inside one.
local function cut(s, most)
  if #s <= most then return s end

  local n = most

  while n > 0 and ((s:byte(n + 1) or 0) & 0xC0) == 0x80 do n = n - 1 end

  return s:sub(1, n)
end

local function text_of(v)
  if v == nil then return nil end

  v = tostring(v):gsub("^%s+", ""):gsub("%s+$", "")

  if v == "" then return nil end

  return cut(v, TEXT_MOST)
end

-- "3", "3/12" or 3: the number, and the total when it is given.
local function count_of(v)
  if type(v) == "number" then return math.tointeger(v) or v end
  if type(v) ~= "string" then return nil end

  local n, total = v:match("^%s*(%d+)%s*/%s*(%d+)")

  if n then return tonumber(n), tonumber(total) end

  return tonumber(v:match("^%s*(%d+)"))
end

local function flag_of(v)
  return v == true or v == 1 or v == "1" or v == "true" or v == "yes"
end

-- Compared without regard to case: what a person typed or a tagger spelt.
local function fold(s) return tostring(s or ""):lower() end

local function extension(name)
  return (name:match("%.([^./]+)$") or ""):lower()
end

local function parent(path) return files.parent(path) end

local function name_of(path) return path:match("([^/]+)$") or path end

local function under(path, folder)
  return path:sub(1, #folder + 1) == folder .. "/"
end

--
-- The format a file is in, when the tag reader did not say: from its
-- extension, where the extension decides it. An `.m4a` is AAC or ALAC and
-- only its sample entry knows which, so it is left unsaid.
--
local FORMAT_BY_EXTENSION = { mp3 = "MP3", flac = "FLAC", wav = "WAV",
                              aif = "AIFF", aiff = "AIFF", aifc = "AIFF",
                              ogg = "Vorbis", oga = "Vorbis", opus = "Opus" }

local LOSSLESS = { FLAC = true, ALAC = true, WAV = true, AIFF = true, PCM = true }

-- A disc's own folder inside its album's: `CD1`, `Disc 2`.
local function disc_folder(name)
  local n = fold(name)

  return n:match("^cd%s*%d+$") or n:match("^dis[ck]%s*%d+$")
end

--------------------------------------------------------------------------
-- A song, from what the file said or from what was kept.
--------------------------------------------------------------------------

--
-- **What `tags.lua` gives, as a song's facts.** Today: title, artist,
-- album, genre, year, track, cover. From P2 (`player.md`): album_artist,
-- composer, disc, compilation, and the stream's format, rate, bits,
-- channels and length, and ReplayGain's - under the names below, and
-- `duration` in seconds is taken as well as `duration_ms`.
--
local function facts_from_tags(t)
  t = type(t) == "table" and t or {}

  local f = {}

  for _, k in ipairs(TEXT) do f[k] = text_of(t[k]) end

  local track, tracks = count_of(t.track)
  local disc, discs = count_of(t.disc)

  f.year = count_of(t.year)
  f.track, f.tracks = track, count_of(t.tracks) or tracks
  f.disc, f.discs = disc, count_of(t.discs) or discs

  for _, k in ipairs({ "rate", "bits", "channels", "gain_track", "peak_track",
                       "gain_album", "peak_album" }) do
    f[k] = tonumber(t[k])
  end

  f.duration_ms = tonumber(t.duration_ms)
                  or (tonumber(t.duration) and math.floor(tonumber(t.duration) * 1000 + 0.5))

  if type(t.cover) == "table" and tonumber(t.cover.offset) and tonumber(t.cover.bytes) then
    f.cover_offset = tonumber(t.cover.offset)
    f.cover_bytes = tonumber(t.cover.bytes)
    f.cover_mime = text_of(t.cover.mime)
  end

  f.compilation = flag_of(t.compilation) or nil

  return f
end

local function facts_from_attrs(attrs)
  local f = {}

  for _, k in ipairs(TEXT) do f[k] = text_of(attrs[key(k)]) end
  for _, k in ipairs(NUMBER) do f[k] = tonumber(attrs[key(k)]) end

  f.compilation = flag_of(attrs[key("compilation")]) or nil

  return f
end

local function person_from_attrs(attrs)
  local p = {}

  for _, k in ipairs(PERSON) do p[k] = tonumber(attrs[key(k)]) end

  return p
end

--
-- The row every view and every rule reads. The fields a person sees left
-- unsaid stay nil here, and the window chooses the words for them; the
-- ones the library needs to group are filled: a song with no title is its
-- file's name, as Tracker shows it.
--
local function make_song(path, facts, person, attrs)
  local s = { path = path, folder = parent(path), size = attrs.size,
              stamp = tonumber(attrs.mtime), modified = attrs.modified }

  for k, v in pairs(facts) do s[k] = v end
  for k, v in pairs(person) do s[k] = v end

  s.title = s.title or (name_of(path):gsub("%.[^.]*$", ""))
  s.format = s.format or FORMAT_BY_EXTENSION[extension(path)]
  s.lossless = LOSSLESS[s.format or ""] or false
  s.plays, s.skips, s.rating = s.plays or 0, s.skips or 0, s.rating or 0

  return s
end

--------------------------------------------------------------------------
-- Writing the store.
--------------------------------------------------------------------------

--
-- Changes, a kilobyte's worth at a time: the disk takes a request's changes
-- in `DISK_DATA_MAX`, so twenty facts with long titles go as two or three.
-- `""` takes an attribute out (`diskfs.c`, `merge_attrs`).
--
local function store(path, changes)
  local batch, used, names = {}, 0, {}

  for k in pairs(changes) do names[#names + 1] = k end

  table.sort(names)

  local function send()
    if next(batch) == nil then return true end

    local ok, why = fs.setattr(path, batch)

    batch, used = {}, 0

    return ok, why
  end

  for _, k in ipairs(names) do
    local v = changes[k]
    local cost = #k + #tostring(v) + 8

    if used + cost > CHANGES_MOST then
      local ok, why = send()

      if not ok then return nil, why end
    end

    batch[k], used = v, used + cost
  end

  return send()
end

-- Every fact of a file read again, the ones it no longer says taken out.
local function store_facts(path, facts, attrs, added)
  local changes = {}

  for _, list in ipairs({ TEXT, NUMBER, FLAG }) do
    for _, k in ipairs(list) do
      local v = facts[k]

      if v == nil then
        if attrs[key(k)] ~= nil then changes[key(k)] = "" end
      else
        changes[key(k)] = v
      end
    end
  end

  if added then changes[key("added")] = added end

  changes[READ_MTIME] = tonumber(attrs.mtime) or ""
  changes[READ_SIZE] = tonumber(attrs.size) or ""

  return store(path, changes)
end

--------------------------------------------------------------------------
-- The library.
--------------------------------------------------------------------------

local Library = {}
Library.__index = Library

--
-- **The default tag reader**: a file's path to what `tags.lua` reads of it.
-- Through `media.tags` today, which is the one door that reads a file's
-- start through a page for `tags.read`; `player.md` moves that door into
-- `tags.lua` itself, and `tags.file` is taken first once it is there.
--
local function default_tags_of(path)
  local tags = use("/Kosmos/Libraries/tags.lua")

  if tags.file then return tags.file(path) end

  return use("/Kosmos/Libraries/media.lua").tags(path)
end

--
-- A library over `folders`. Nothing is read until `refresh` or `start`.
--
--   folders    the folders to walk, each with every folder inside it
--   kinds      the extensions that count, lower case; `library.MUSIC`
--   tags_of    path -> the table `tags.read` gives, or nil and why
--   now        () -> seconds since 1970, for a play and a day added
--   playlists  the folder of playlists; `library.PLAYLISTS`
--
function library.open(o)
  o = o or {}

  local self = setmetatable({
    roots = {},
    kinds = o.kinds or library.MUSIC,
    tags_of = o.tags_of or default_tags_of,
    now = o.now or function() return nil end,
    playlist_dir = o.playlists or library.PLAYLISTS,
    by_path = {},          -- every song, by its path
    covers = {},           -- a folder's own picture, by the folder
    walked = {},           -- every folder the last refresh listed
    dirty = true,          -- albums and artists to be grouped again
  }, Library)

  for i, f in ipairs(o.folders or { "/Home/Music" }) do
    self.roots[i] = (f:gsub("/$", ""))
  end

  return self
end

--------------------------------------------------------------------------
-- Refreshing, a slice at a time.
--------------------------------------------------------------------------

--
-- **A refresh begun**: of every folder, or of only `folders` - the one a
-- folder's pulse named, say (`win:watch_folder`, `pulses.md` P4). Songs
-- under a folder walked and not found there are gone when it ends.
--
function Library:start(folders)
  local list = {}

  for i, f in ipairs(folders or self.roots) do list[i] = (f:gsub("/$", "")) end

  self.pass = {
    folders = { table.unpack(list) },   -- still to be listed
    tops = list,
    files = {},             -- found, still to be looked at
    seen = {},
    counts = { folders = 0, seen = 0, read = 0, kept = 0, removed = 0,
               failed = 0, unstored = 0 },
  }

  -- What is under them is listed afresh, so a folder taken away is not
  -- watched again.
  for f in pairs(self.walked) do
    for _, top in ipairs(list) do
      if f == top or under(f, top) then self.walked[f] = nil end
    end
  end

  return self.pass.counts
end

-- One folder listed: its songs queued, the folders in it queued after.
function Library:list_folder(folder)
  local pass = self.pass
  local entries = files.entries(folder)

  pass.counts.folders = pass.counts.folders + 1

  if not entries then return end

  self.walked[folder] = true
  self.covers[folder] = nil

  local best

  for _, e in ipairs(entries) do
    local path = files.join(folder, e.name)

    if e.name:sub(1, 1) == "." then
      -- hidden: not the library's
    elseif e.kind == "directory" then
      if path ~= self.playlist_dir then pass.folders[#pass.folders + 1] = path end
    elseif self.kinds[extension(e.name)] then
      pass.files[#pass.files + 1] = { path = path, attrs = e.attrs or {} }
    else
      local rank = FOLDER_COVERS[e.name:lower()]

      if rank and (not best or rank < best) then
        best = rank
        self.covers[folder] = path
      end
    end
  end
end

--
-- **One file looked at.** Its facts from memory when this process read it
-- at this stamp and size; from its attributes when they were written at
-- this stamp and size; and only otherwise read with the tag reader - the
-- one expensive thing a refresh does, and the thing the host test counts.
--
function Library:look_at(path, attrs)
  local pass = self.pass
  local counts = pass.counts
  local stamp, size = tonumber(attrs.mtime), tonumber(attrs.size)
  local known = self.by_path[path]

  pass.seen[path] = true
  counts.seen = counts.seen + 1

  -- Read at this stamp and size: by this process, or by whoever wrote the
  -- facts beside it. A file with no stamp at all is compared by its size
  -- and the date the drive gives it, and read again if it gives neither.
  local same_here = known and known.size == size
                    and ((stamp and known.stamp == stamp)
                         or (not stamp and attrs.modified and known.modified == attrs.modified))
  local same_kept = stamp ~= nil
                    and tonumber(attrs[READ_MTIME]) == stamp
                    and tonumber(attrs[READ_SIZE]) == size

  if same_kept then
    self.by_path[path] = make_song(path, facts_from_attrs(attrs),
                                   person_from_attrs(attrs), attrs)
    counts.kept = counts.kept + 1
    self.dirty = true
    return
  end

  if same_here then
    counts.kept = counts.kept + 1
    return
  end

  counts.read = counts.read + 1

  local ok, t, why = pcall(self.tags_of, path)

  if not ok or t == nil then
    counts.failed = counts.failed + 1
    t = {}
  end

  local facts = facts_from_tags(t)
  local person = person_from_attrs(attrs)

  facts.format = facts.format or FORMAT_BY_EXTENSION[extension(path)]

  -- The day it arrived: kept from the first time, or the day the disk made
  -- it, or today.
  local added

  if not person.added then
    added = tonumber(attrs.created) or tonumber(attrs.modified) or self.now()
    person.added = added
  end

  local stored = store_facts(path, facts, attrs, added)

  if not stored then counts.unstored = counts.unstored + 1 end

  local s = make_song(path, facts, person, attrs)

  s.stored = stored and true or false
  self.by_path[path] = s
  self.dirty = true
end

--
-- **At most `budget` things done** - a folder listed or a file read is one;
-- a file whose facts are kept costs nothing worth counting, since its
-- attributes came with the listing. True when the refresh is over, with
-- what it did.
--
function Library:step(budget)
  local pass = self.pass

  if not pass then return true, nil end

  budget = budget or 8

  while budget > 0 do
    if #pass.files > 0 then
      local f = table.remove(pass.files)
      local before = pass.counts.read

      self:look_at(f.path, f.attrs)

      if pass.counts.read > before then budget = budget - 1 end
    elseif #pass.folders > 0 then
      self:list_folder(table.remove(pass.folders, 1))
      budget = budget - 1
    else
      self:finish()
      return true, pass.counts
    end
  end

  return false, pass.counts
end

-- The songs under the folders walked and not found there, let go.
function Library:finish()
  local pass = self.pass

  for path in pairs(self.by_path) do
    if not pass.seen[path] then
      for _, top in ipairs(pass.tops) do
        if path == top or under(path, top) then
          self.by_path[path] = nil
          pass.counts.removed = pass.counts.removed + 1
          self.dirty = true
          break
        end
      end
    end
  end

  self.pass = nil
end

--
-- A whole refresh in one call: `start`, then `step` until it is over, with
-- `on_progress(counts)` after each slice. Returns what it did.
--
function Library:refresh(on_progress, folders)
  self:start(folders)

  while true do
    local done, counts = self:step(16)

    if on_progress then on_progress(counts) end
    if done then return counts end
  end
end

-- Every folder the refreshes listed, for a window to watch.
function Library:folders()
  local out = {}

  for f in pairs(self.walked) do out[#out + 1] = f end

  table.sort(out)
  return out
end

--------------------------------------------------------------------------
-- Albums and artists.
--------------------------------------------------------------------------

--
-- The folder an album lives in: its songs' folder, or the one above it when
-- that is a disc's own (`CD1`, `Disc 2`).
--
local function album_folder(song)
  local folder = song.folder

  if disc_folder(name_of(folder)) then return parent(folder) end

  return folder
end

local function various(name)
  local n = fold(name)

  return n == "various artists" or n == "various" or n == "va"
end

--
-- **An album's key.** A compilation - the file says so (`cpil`,
-- `COMPILATION=1`), or its album artist is *Various artists* - is the
-- album's name alone in its folder. Otherwise the album artist where the
-- file names one, and the album's name. A song with no album is its
-- folder's.
--
-- **And an album the files forgot to call a compilation**: songs of one
-- name in one folder, with no album artist and more than one artist, are
-- one compilation rather than twenty albums of a song each (`player.md`,
-- *An album opened*). Two albums of one name by two artists in two folders
-- stay two.
--
local function group_songs(songs)
  local loose = {}        -- album name and folder -> artists, for the second look

  for _, s in pairs(songs) do
    if s.album and not s.album_artist and not s.compilation then
      local k = fold(s.album) .. "\0" .. album_folder(s)
      local seen = loose[k] or {}

      seen[fold(s.artist)] = true
      loose[k] = seen
    end
  end

  local albums = {}

  for _, s in pairs(songs) do
    local id, artist, compilation

    if not s.album then
      id = "folder\0" .. s.folder
    elseif s.compilation or various(s.album_artist) then
      id, compilation = "various\0" .. fold(s.album) .. "\0" .. album_folder(s), true
    elseif s.album_artist then
      id, artist = "artist\0" .. fold(s.album_artist) .. "\0" .. fold(s.album), s.album_artist
    else
      local seen = loose[fold(s.album) .. "\0" .. album_folder(s)]
      local n = 0

      for _ in pairs(seen) do n = n + 1 end

      if n > 1 then
        id, compilation = "various\0" .. fold(s.album) .. "\0" .. album_folder(s), true
      else
        id, artist = "artist\0" .. fold(s.artist) .. "\0" .. fold(s.album), s.artist
      end
    end

    local a = albums[id]

    if not a then
      a = { id = id, title = s.album or name_of(s.folder), artist = artist,
            compilation = compilation or false, folder = album_folder(s),
            songs = {}, formats = {} }
      albums[id] = a
    end

    a.songs[#a.songs + 1] = s
    s.album_id = id
    s.on_compilation = a.compilation
  end

  return albums
end

local function by_disc_and_track(x, y)
  if (x.disc or 1) ~= (y.disc or 1) then return (x.disc or 1) < (y.disc or 1) end
  if (x.track or 0) ~= (y.track or 0) then return (x.track or 0) < (y.track or 0) end
  if fold(x.title) ~= fold(y.title) then return fold(x.title) < fold(y.title) end
  return x.path < y.path
end

--
-- What an album says of itself, from its songs: its year, its length, its
-- format - all lossless or not, the highest rate and depth, which is what
-- *Hi-Res 96/24* is drawn from - and its cover: the first song's own
-- picture in track order, or the folder's.
--
local function summarise(a, covers)
  table.sort(a.songs, by_disc_and_track)

  a.count, a.duration_ms, a.lossless, a.added = #a.songs, 0, true, nil

  for _, s in ipairs(a.songs) do
    a.duration_ms = a.duration_ms + (s.duration_ms or 0)
    a.lossless = a.lossless and s.lossless
    a.rate = math.max(a.rate or 0, s.rate or 0)
    a.bits = math.max(a.bits or 0, s.bits or 0)
    a.year = a.year or s.year
    a.genre = a.genre or s.genre
    a.added = math.max(a.added or 0, s.added or 0)

    if s.format then a.formats[s.format] = true end

    if not a.cover and s.cover_offset and s.cover_bytes then
      a.cover = { path = s.path, offset = s.cover_offset, bytes = s.cover_bytes,
                  mime = s.cover_mime }
    end
  end

  if a.rate == 0 then a.rate = nil end
  if a.bits == 0 then a.bits = nil end
  if a.added == 0 then a.added = nil end

  local only, n = nil, 0

  for f in pairs(a.formats) do only, n = f, n + 1 end

  a.format = (n == 1) and only or nil

  if not a.cover then
    local picture = covers[a.folder]

    for _, s in ipairs(a.songs) do picture = picture or covers[s.folder] end

    if picture then a.cover = { path = picture } end
  end
end

local function by_title(x, y)
  if fold(x.title) ~= fold(y.title) then return fold(x.title) < fold(y.title) end
  return x.id < y.id
end

--
-- **Artists, by album artist where the file names one** (`player.md`). An
-- artist's albums are the ones that are theirs; a compilation is no
-- artist's album, and is listed as one each of its artists *appears on*.
--
function Library:group()
  if not self.dirty then return end

  local albums = group_songs(self.by_path)
  local artists = {}

  local function artist(name)
    local k = fold(name)
    local a = artists[k]

    if not a then
      a = { name = name, albums = {}, appears_on = {}, songs = 0, has = {} }
      artists[k] = a
    end

    return a
  end

  local list = {}

  for _, a in pairs(albums) do
    summarise(a, self.covers)
    list[#list + 1] = a

    if a.artist then
      table.insert(artist(a.artist).albums, a)
    end

    for _, s in ipairs(a.songs) do
      local name = (not a.compilation and s.album_artist) or s.artist

      if name then
        local who = artist(name)

        who.songs = who.songs + 1

        if a.compilation and not who.has[a.id] then
          who.has[a.id] = true
          table.insert(who.appears_on, a)
        end
      end
    end
  end

  table.sort(list, by_title)

  local names = {}

  for _, a in pairs(artists) do
    table.sort(a.albums, by_title)
    table.sort(a.appears_on, by_title)
    a.has = nil
    names[#names + 1] = a
  end

  table.sort(names, function(x, y) return fold(x.name) < fold(y.name) end)

  self.album_list, self.album_by_id = list, albums
  self.artist_list, self.artist_by_name = names, artists
  self.dirty = false
end

-- Every album, by title.
function Library:albums()
  self:group()
  return self.album_list
end

function Library:album(id)
  self:group()
  return self.album_by_id[id]
end

-- Every artist, by name.
function Library:artists()
  self:group()
  return self.artist_list
end

function Library:artist(name)
  self:group()
  return self.artist_by_name[fold(name)]
end

function Library:song(path)
  return self.by_path[path]
end

--------------------------------------------------------------------------
-- Songs, sorted.
--------------------------------------------------------------------------

--
-- **The column sort**: by any field, ties broken as a person expects - by
-- album artist, album, disc and track - so sorting by genre still reads as
-- albums in order.
--
local function song_order(field, descending)
  local function value(s)
    if field == "artist" then return fold(s.album_artist or s.artist) end

    local v = s[field]

    if type(v) == "string" then return fold(v) end
    if type(v) == "boolean" then return v and 1 or 0 end

    return v
  end

  local function then_album(x, y)
    local ax, ay = fold(x.album_artist or x.artist), fold(y.album_artist or y.artist)

    if ax ~= ay then return ax < ay end
    if fold(x.album) ~= fold(y.album) then return fold(x.album) < fold(y.album) end

    return by_disc_and_track(x, y)
  end

  return function(x, y)
    local vx, vy = value(x), value(y)

    if vx ~= vy then
      -- Unknown is last whichever way the column runs.
      if vx == nil then return false end
      if vy == nil then return true end
      if type(vx) ~= type(vy) then vx, vy = tostring(vx), tostring(vy) end

      if descending then return vx > vy end
      return vx < vy
    end

    return then_album(x, y)
  end
end

local function all_songs(self)
  local out = {}

  for _, s in pairs(self.by_path) do out[#out + 1] = s end

  return out
end

--
-- Songs as rows, a page at a time: `sort` (a field), `descending`, `where`
-- (a rule, as `smart` takes), `from` and `count`. Returns the page and how
-- many there are in all - or nil and why, for a rule that is not one.
--
function Library:songs(o)
  o = o or {}

  local rows = all_songs(self)

  if o.where then
    local chosen, why = self:smart(o.where, o.now)

    if not chosen then return nil, why end

    rows = chosen
  end

  if not (o.where and o.where.by and not o.sort) then
    table.sort(rows, song_order(o.sort or "artist", o.descending))
  end

  local from, count = o.from or 1, o.count or #rows
  local page = {}

  for i = from, math.min(#rows, from + count - 1) do page[#page + 1] = rows[i] end

  return page, #rows
end

--------------------------------------------------------------------------
-- Search.
--------------------------------------------------------------------------

local SEARCHED = { "title", "artist", "album", "album_artist", "composer", "genre" }

local function words_of(text)
  local out = {}

  for w in fold(text):gmatch("%S+") do out[#out + 1] = w end

  return out
end

local function has_all(hay, words)
  for _, w in ipairs(words) do
    if not hay:find(w, 1, true) then return false end
  end

  return true
end

--
-- **As you type** (`player.md`, Search): every word somewhere among the
-- song's title, artist, album, album artist, composer and genre - "night
-- fe" finds *Night Fever* - and the results grouped as songs, albums and
-- artists. A substring scan over the rows in memory; ten thousand songs is
-- a few milliseconds of it.
--
function Library:search(text)
  self:group()

  local words = words_of(text)
  local out = { songs = {}, albums = {}, artists = {} }

  if #words == 0 then return out end

  for _, s in pairs(self.by_path) do
    if not s.hay then
      local parts = {}

      for i, f in ipairs(SEARCHED) do parts[i] = fold(s[f]) end

      s.hay = table.concat(parts, "\0")
    end

    if has_all(s.hay, words) then out.songs[#out.songs + 1] = s end
  end

  for _, a in ipairs(self.album_list) do
    if has_all(fold(a.title) .. "\0" .. fold(a.artist), words) then
      out.albums[#out.albums + 1] = a
    end
  end

  for _, a in ipairs(self.artist_list) do
    if has_all(fold(a.name), words) then out.artists[#out.artists + 1] = a end
  end

  table.sort(out.songs, song_order("artist"))

  return out
end

--------------------------------------------------------------------------
-- Smart rules.
--------------------------------------------------------------------------

--
-- **A rule is a table, and nothing else**, so a smart playlist is a file
-- holding one and Automations can write one:
--
--   { match = "all",                         -- or "any"
--     rules = {
--       { field = "genre", op = "is", value = "Jazz" },
--       { field = "added", op = "in_last", value = 30 },       -- days
--       { field = "playlist", op = "in", value = "Road" },
--       { match = "any", rules = { ... } },                    -- a group
--     },
--     by = "plays", descending = true, limit = 25 }            -- 25, most played
--
-- The fields are `player.md`'s - artist, genre, year, format, rate, bits,
-- rating, plays, last played, date added, a playlist - and every other fact
-- a song has. Text compares without regard to case; a play count, skips
-- and a rating that were never set are 0; a date never set is never, so
-- *not played in the last 30 days* takes a song never played.
--
-- **Checked before it runs**: a field or an operation there is no such
-- thing as is refused with its name, rather than choosing nothing in
-- silence.
--
local FIELDS = {
  title = "text", artist = "text", album = "text", album_artist = "text",
  composer = "text", genre = "text", format = "text", codec = "text",
  path = "text", folder = "text",
  year = "number", track = "number", disc = "number", rate = "number",
  bits = "number", channels = "number", duration_ms = "number",
  size = "number", rating = "number", plays = "number", skips = "number",
  played = "date", added = "date",
  lossless = "flag", compilation = "flag",
  playlist = "playlist",
}

local OPS = {
  text = { is = true, is_not = true, contains = true, lacks = true, starts = true },
  number = { is = true, is_not = true, ["<"] = true, ["<="] = true, [">"] = true,
             [">="] = true, between = true },
  date = { ["<"] = true, ["<="] = true, [">"] = true, [">="] = true,
           between = true, in_last = true, not_in_last = true },
  flag = { is = true, is_not = true },
  playlist = { ["in"] = true, not_in = true },
}

library.FIELDS = FIELDS

local function check_rule(rule, depth)
  if type(rule) ~= "table" then return nil, "a rule is a table" end
  if depth > 8 then return nil, "rules inside rules, too deep" end

  if rule.rules ~= nil then
    if type(rule.rules) ~= "table" then return nil, "rules is a list" end

    local m = rule.match or "all"

    if m ~= "all" and m ~= "any" then
      return nil, ("match is all or any, not %s"):format(tostring(m))
    end

    for _, r in ipairs(rule.rules) do
      local ok, why = check_rule(r, depth + 1)

      if not ok then return nil, why end
    end

    if rule.by ~= nil and not FIELDS[rule.by] then
      return nil, ("there is no field %s to order by"):format(tostring(rule.by))
    end

    if rule.limit ~= nil and not (math.tointeger(rule.limit) and rule.limit > 0) then
      return nil, "a limit is a number of songs"
    end

    return true
  end

  local kind = FIELDS[rule.field]

  if not kind then return nil, ("there is no field %s"):format(tostring(rule.field)) end

  if not OPS[kind][rule.op] then
    return nil, ("%s does not take %s"):format(rule.field, tostring(rule.op))
  end

  if rule.op == "between" then
    if type(rule.value) ~= "table" or tonumber(rule.value[1]) == nil
       or tonumber(rule.value[2]) == nil then
      return nil, "between takes two numbers"
    end
  elseif kind == "number" or kind == "date" then
    if tonumber(rule.value) == nil then
      return nil, ("%s %s takes a number"):format(rule.field, rule.op)
    end
  elseif kind == "flag" then
    if type(rule.value) ~= "boolean" then
      return nil, ("%s is true or false"):format(rule.field)
    end
  elseif rule.value == nil then
    return nil, ("%s %s takes a value"):format(rule.field, rule.op)
  end

  return true
end

-- Whether one song is chosen by one rule, or a group of them.
local function chooses(self, rule, s, now, lists)
  if rule.rules then
    local any = rule.match == "any"

    for _, r in ipairs(rule.rules) do
      local yes = chooses(self, r, s, now, lists)

      if any and yes then return true end
      if not any and not yes then return false end
    end

    -- An empty group: all of nothing is everything, any of nothing is none.
    return not any
  end

  local kind, op, want = FIELDS[rule.field], rule.op, rule.value

  if kind == "playlist" then
    local name = tostring(want)

    if lists[name] == nil then
      local set = {}

      for _, p in ipairs(self:playlist(name) or {}) do set[p] = true end

      lists[name] = set
    end

    local member = lists[name][s.path] == true

    if op == "in" then return member end
    return not member
  end

  -- *Is a compilation* is the album's word, not only the file's: a
  -- compilation the files forgot to call one is one here too.
  local have = (rule.field == "compilation") and s.on_compilation or s[rule.field]

  if kind == "text" then
    local h, w = fold(have), fold(want)

    if op == "is" then return have ~= nil and h == w end
    if op == "is_not" then return have == nil or h ~= w end
    if op == "contains" then return have ~= nil and h:find(w, 1, true) ~= nil end
    if op == "lacks" then return have == nil or h:find(w, 1, true) == nil end
    if op == "starts" then return have ~= nil and h:sub(1, #w) == w end
  end

  if kind == "flag" then
    if op == "is" then return (have == true) == want end
    return (have == true) ~= want
  end

  -- Dates: never set is never.
  if op == "in_last" or op == "not_in_last" then
    local within = have ~= nil and now ~= nil
                   and have >= now - tonumber(want) * SECONDS_A_DAY

    if op == "in_last" then return within end
    return not within
  end

  if have == nil then return op == "is_not" end

  if op == "between" then
    local lo, hi = tonumber(want[1]), tonumber(want[2])

    if lo > hi then lo, hi = hi, lo end

    return have >= lo and have <= hi
  end

  want = tonumber(want)

  if op == "is" then return have == want end
  if op == "is_not" then return have ~= want end
  if op == "<" then return have < want end
  if op == "<=" then return have <= want end
  if op == ">" then return have > want end
  if op == ">=" then return have >= want end

  return false
end

--
-- **The songs a rule chooses**, in its `by` order (or the library's) and
-- cut to its `limit`. `now` is seconds since 1970, for *in the last n
-- days*; the library's clock when not given. Nil and why for a rule that
-- is not one.
--
function Library:smart(rule, now)
  local ok, why = check_rule(rule, 0)

  if not ok then return nil, why end

  -- A single condition is a group of one.
  if not rule.rules then rule = { match = "all", rules = { rule } } end

  now = now or self.now()
  self:group()

  local out, lists = {}, {}

  for _, s in pairs(self.by_path) do
    if chooses(self, rule, s, now, lists) then out[#out + 1] = s end
  end

  table.sort(out, song_order(rule.by or "artist", rule.descending))

  if rule.limit then
    for i = #out, rule.limit + 1, -1 do out[i] = nil end
  end

  return out
end

--
-- **The five that ship** (`player.md`, Smart playlists). *Hi-Res* is
-- lossless at 88.2 kHz or more, or lossless at 24 bits.
--
library.SMART = {
  { name = "Recently added",
    rule = { match = "all", rules = { { field = "added", op = "in_last", value = 30 } },
             by = "added", descending = true } },
  { name = "Most played",
    rule = { match = "all", rules = { { field = "plays", op = ">", value = 0 } },
             by = "plays", descending = true, limit = 25 } },
  { name = "Top rated",
    rule = { match = "all", rules = { { field = "rating", op = ">=", value = 4 } },
             by = "rating", descending = true } },
  { name = "Hi-Res",
    rule = { match = "all", rules = {
               { field = "lossless", op = "is", value = true },
               { match = "any", rules = { { field = "rate", op = ">=", value = 88200 },
                                          { field = "bits", op = ">=", value = 24 } } } } } },
  { name = "Never played",
    rule = { match = "all", rules = { { field = "plays", op = "is", value = 0 } } } },
}

--------------------------------------------------------------------------
-- Playlists.
--------------------------------------------------------------------------

--
-- **A playlist is a small file in the playlists folder** (decision 3),
-- named as the playlist is, holding `{ songs = { path, ... } }` as
-- `fs.write` stores a table. Read here for *is in a playlist*; making,
-- renaming and reordering them is P8's.
--
function Library:playlists()
  local names = fs.list(self.playlist_dir) or {}
  local out = {}

  for _, n in ipairs(names) do
    if n:sub(1, 1) ~= "." then out[#out + 1] = n end
  end

  table.sort(out, function(x, y) return fold(x) < fold(y) end)
  return out
end

function Library:playlist(name)
  local t = fs.read(files.join(self.playlist_dir, name))

  if type(t) ~= "table" or type(t.songs) ~= "table" then return nil end

  local out = {}

  for _, p in ipairs(t.songs) do
    if type(p) == "string" then out[#out + 1] = p end
  end

  return out
end

--------------------------------------------------------------------------
-- What the person did.
--------------------------------------------------------------------------

local function bump(self, path, changes)
  local s = self.by_path[path]

  if not s then return nil, "not in the library: " .. tostring(path) end

  local attrs = {}

  for k, v in pairs(changes) do
    s[k] = v
    attrs[key(k)] = (v == 0 and k == "rating") and "" or v
  end

  if s.stored == false then return true end

  return store(path, attrs)
end

--
-- **A play counted when the song finished** (`player.md`, as iTunes counts
-- it) - the caller says when that was, or the library's clock does.
--
function Library:played(path, when)
  local s = self.by_path[path]

  if not s then return nil, "not in the library: " .. tostring(path) end

  return bump(self, path, { plays = (s.plays or 0) + 1, played = when or self.now() })
end

function Library:skipped(path)
  local s = self.by_path[path]

  if not s then return nil, "not in the library: " .. tostring(path) end

  return bump(self, path, { skips = (s.skips or 0) + 1 })
end

-- One to five stars, or 0 for none.
function Library:rate(path, stars)
  stars = math.tointeger(stars)

  if not stars or stars < 0 or stars > 5 then return nil, "a rating is 0 to 5 stars" end

  return bump(self, path, { rating = stars })
end

return library
