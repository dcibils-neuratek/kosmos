-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The Library kit, on this machine (`user/lib/library.lua`, `docs/player.md` P4).
--
--   build/host/lua tools/test_library.lua [library.lua]
--
-- A tree of songs made here - MP3s with ID3v2.3 tags built as the format
-- says, WAVs with a `LIST INFO` chunk, FLACs - under an `fs` kept in
-- memory that keeps attributes as `/Home` does: a `setattr` merges, `""`
-- takes one out, a request's changes past a kilobyte are refused, and the
-- stamp does not move. A second root stands for a FAT stick, which keeps no
-- attributes at all.
--
-- The files are read through the real `tags.lua`. **What it does not read
-- yet** - a FLAC's Vorbis comments, an album artist, a disc, the stream's
-- rate and depth, all P2's - is handed in beside it, under the names the
-- kit expects `tags.lua` to give them (`extra` below), so the grouping and
-- the rules are held now and the same checks hold when P2 lands.
--
-- Held: the albums and their counts, compilations grouped two ways, discs
-- in order, artists by album artist; the attributes written, and a file
-- read again only when its stamp or size moved (reads counted, per file);
-- search; every smart rule against the songs it must choose.
--
-- **The control** (`player.md` P4): a refresh that ignores the stamps reads
-- every file every time, and the read counts here fail. Run this against a
-- copy of the kit with the comparison taken out to see it.

local kit_path = arg and arg[1] or "user/lib/library.lua"

use = use or function(path) return dofile((path:gsub("^/Kosmos/Libraries/", "user/lib/"))) end

local tags = assert(loadfile("user/lib/tags.lua"))()
local library = assert(loadfile(kit_path))()

local passed, failed = 0, 0

local function check(ok, what)
  if ok then
    passed = passed + 1
  else
    failed = failed + 1
    print("  FAIL: " .. what)
  end
end

local function same_set(got, want, what)
  local g, w = {}, {}

  for _, x in ipairs(got or {}) do g[x] = true end
  for _, x in ipairs(want) do w[x] = true end

  local ok = #(got or {}) == #want

  for x in pairs(w) do if not g[x] then ok = false end end

  if not ok then
    local list = {}

    for _, x in ipairs(got or {}) do list[#list + 1] = x end

    table.sort(list)
    what = what .. " - got {" .. table.concat(list, ", ") .. "}"
  end

  check(ok, what)
end

--------------------------------------------------------------------------
-- Files, made as their formats say.
--------------------------------------------------------------------------

local function syncsafe(n)
  return string.char((n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F)
end

local function frame(id, data)
  return id .. string.pack(">I4I2", #data, 0) .. data
end

-- An MP3: an ID3v2.3 tag of the fields given, then a frame's worth of
-- nothing for a body. `pad` makes one longer without changing what it says.
local function mp3(t, pad)
  local body = ""
  local ids = { title = "TIT2", artist = "TPE1", album = "TALB", track = "TRCK",
                year = "TYER", genre = "TCON" }

  for _, f in ipairs({ "title", "artist", "album", "track", "year", "genre" }) do
    if t[f] then body = body .. frame(ids[f], "\0" .. t[f]) end
  end

  if t.picture then
    body = body .. frame("APIC", "\0image/jpeg\0\3\0" .. t.picture)
  end

  local tag = (body == "") and "" or ("ID3" .. string.char(3, 0, 0) .. syncsafe(#body) .. body)

  return tag .. "\255\251\144\0" .. ("\0"):rep(64 + (pad or 0))
end

-- A WAV with an INFO chunk.
local function wav(t)
  local info = "INFO"
  local ids = { title = "INAM", artist = "IART", album = "IPRD" }

  for _, f in ipairs({ "title", "artist", "album" }) do
    if t[f] then
      local s = t[f] .. "\0"

      if #s % 2 == 1 then s = s .. "\0" end

      info = info .. ids[f] .. string.pack("<I4", #s) .. s
    end
  end

  local list = "LIST" .. string.pack("<I4", #info) .. info
  local data = "data" .. string.pack("<I4", 16) .. ("\0"):rep(16)

  return "RIFF" .. string.pack("<I4", 4 + #list + #data) .. "WAVE" .. list .. data
end

local function flac() return "fLaC" .. ("\0"):rep(60) end

--------------------------------------------------------------------------
-- An fs in memory, keeping attributes as `/Home` keeps them.
--------------------------------------------------------------------------

local T0 = 1795000000                 -- "now", 18 November 2026
local DAY = 86400

local tree = {}                        -- path -> { dir = true } or a file
local setattr_calls = {}               -- path -> how many requests
local NO_ATTRS = "/Drive"              -- a FAT stick: no attributes kept

local function add_dirs(path)
  local p = path:match("^(.*)/[^/]+$")

  while p and p ~= "" do
    tree[p] = tree[p] or { dir = true }
    p = p:match("^(.*)/[^/]+$")
  end
end

local stamp = 1000

local function put(path, bytes, o)
  o = o or {}
  add_dirs(path)

  local old = tree[path]

  stamp = stamp + 1
  tree[path] = { bytes = bytes, mtime = o.mtime or stamp,
                 created = o.created, modified = o.modified,
                 attrs = (old and old.attrs) or {}, extra = o.extra, data = o.data }
end

fs = {}

function fs.list(dir)
  if not (tree[dir] and tree[dir].dir) then return nil, "no such folder: " .. dir end

  local out = {}
  local pat = "^" .. dir:gsub("%p", "%%%0") .. "/([^/]+)$"

  for path in pairs(tree) do
    local name = path:match(pat)

    if name then out[#out + 1] = name end
  end

  table.sort(out)
  return out
end

function fs.getattr(path)
  local n = tree[path]

  if not n then return nil end
  if n.dir then return { kind = "directory", size = 0 } end

  local out = {}

  for k, v in pairs(n.attrs) do out[k] = v end

  out.kind = out.kind or "file"
  out.size = #(n.bytes or "")
  out.mtime = (path:sub(1, #NO_ATTRS) ~= NO_ATTRS) and n.mtime or nil
  out.created = n.created
  out.modified = n.modified

  return out
end

function fs.setattr(path, changes)
  local n = tree[path]

  if not n then return nil, "no such file" end
  if path:sub(1, #NO_ATTRS) == NO_ATTRS then return nil, "this drive keeps no attributes" end

  local used = 0

  for k, v in pairs(changes) do
    if type(v) == "table" or type(k) ~= "string" then return nil, "not flat" end

    used = used + #k + #tostring(v) + 3
  end

  if used > 1024 then return nil, "more than a request carries" end

  setattr_calls[path] = (setattr_calls[path] or 0) + 1

  for k, v in pairs(changes) do
    if v == "" then n.attrs[k] = nil else n.attrs[k] = v end
  end

  return true
end

function fs.read(path)
  local n = tree[path]

  return n and n.data
end

-- The tag reader, as the machine has it, reads counted.
local reads, reads_of = 0, {}

local function tags_of(path)
  local n = tree[path]

  if not n then return nil, "no such file" end

  reads = reads + 1
  reads_of[path] = (reads_of[path] or 0) + 1

  local bytes = n.bytes
  local t = tags.read(function(offset, k)
    if offset >= #bytes then return nil end
    return bytes:sub(offset + 1, offset + k)
  end, #bytes)

  for k, v in pairs(n.extra or {}) do
    if t[k] == nil then t[k] = v end
  end

  return t
end

local function reset_reads() reads, reads_of = 0, {} end

local function open(folders)
  return library.open{ folders = folders or { "/Home/Music", "/Drive/Stick" },
                       tags_of = tags_of, now = function() return T0 end }
end

--------------------------------------------------------------------------
-- The tree.
--------------------------------------------------------------------------

local M = "/Home/Music"
local OLD = T0 - 100 * DAY

put(M .. "/Ana Sol/Mar/01 Ola.mp3",
    mp3{ title = "Ola", artist = "Ana Sol", album = "Mar", track = "1", year = "2019", genre = "Pop" },
    { created = OLD })
put(M .. "/Ana Sol/Mar/02 Night Fever.mp3",
    mp3{ title = "Night Fever", artist = "Ana Sol", album = "Mar", track = "2/3", year = "2019", genre = "Pop" },
    { created = OLD })
put(M .. "/Ana Sol/Mar/03 Brisa.mp3",
    mp3{ title = "Brisa", artist = "Ana Sol", album = "Mar", track = "3", year = "2019", genre = "pop" },
    { created = OLD })
put(M .. "/Ana Sol/Mar/cover.jpg", "\255\216 a picture", { created = OLD })
put(M .. "/Ana Sol/Mar/notes.txt", "not a song", { created = OLD })

put(M .. "/Ana Sol/Tierra/01.flac", flac(),
    { created = T0 - 2 * DAY,
      extra = { title = "Raiz", artist = "Ana Sol", album = "Tierra", track = "1",
                year = "2021", genre = "Folk", format = "FLAC", rate = 96000, bits = 24,
                channels = 2, duration = 241.5 } })
put(M .. "/Ana Sol/Tierra/02.flac", flac(),
    { created = T0 - 2 * DAY,
      extra = { title = "Hoja", artist = "Ana Sol", album = "Tierra", track = "2",
                year = "2021", genre = "Folk", format = "FLAC", rate = 96000, bits = 24,
                channels = 2, duration_ms = 180000 } })

-- A compilation nobody marked: one album's name, one folder, three artists.
put(M .. "/Hits/01.mp3", mp3{ title = "Uno", artist = "Bo Rey", album = "Summer Hits", track = "1", year = "2018" },
    { created = OLD })
put(M .. "/Hits/02.mp3", mp3{ title = "Dos", artist = "Cy Lun", album = "Summer Hits", track = "2", year = "2018" },
    { created = OLD })
put(M .. "/Hits/03.mp3", mp3{ title = "Tres", artist = "Di Mar", album = "Summer Hits", track = "3", year = "2018" },
    { created = OLD })

-- A compilation that says so, across two discs' folders.
put(M .. "/Dance 2020/CD1/01.mp3",
    mp3{ title = "Alba", artist = "Eva Luz", album = "Dance 2020", track = "1", year = "2020" },
    { created = OLD, extra = { album_artist = "Various Artists", disc = "1/2" } })
put(M .. "/Dance 2020/CD1/02.mp3",
    mp3{ title = "Brillo", artist = "Gil Paz", album = "Dance 2020", track = "2", year = "2020" },
    { created = OLD, extra = { album_artist = "Various Artists", disc = "1/2" } })
put(M .. "/Dance 2020/CD2/01.mp3",
    mp3{ title = "Cielo", artist = "Fer Sur", album = "Dance 2020", track = "1", year = "2020" },
    { created = OLD, extra = { album_artist = "Various Artists", disc = "2/2" } })

-- Two albums of one name, by two artists, in two folders: two albums.
put(M .. "/Bo Rey/Greatest Hits/01.mp3",
    mp3{ title = "Primera", artist = "Bo Rey", album = "Greatest Hits", track = "1" }, { created = OLD })
put(M .. "/Bo Rey/Greatest Hits/02.mp3",
    mp3{ title = "Segunda", artist = "Bo Rey", album = "Greatest Hits", track = "2",
         picture = "\255\216 the cover" }, { created = OLD })
put(M .. "/Cy Lun/Greatest Hits/01.wav", wav{ title = "Luz", artist = "Cy Lun", album = "Greatest Hits" },
    { created = OLD, extra = { track = "1", format = "WAV", rate = 44100, bits = 16 } })
put(M .. "/Cy Lun/Greatest Hits/02.wav", wav{ title = "Sombra", artist = "Cy Lun", album = "Greatest Hits" },
    { created = OLD, extra = { track = "2", format = "WAV", rate = 48000, bits = 24 } })

-- A song that says nothing: its file's name, its folder's album.
put(M .. "/Loose/untagged.mp3", mp3{}, { created = OLD })

-- Not the library's: a hidden folder, and the playlists.
put(M .. "/.hidden/secret.mp3", mp3{ title = "Secret", artist = "Nobody", album = "Hidden" })
put(M .. "/Playlists/Road", "", { data = { songs = { M .. "/Ana Sol/Mar/01 Ola.mp3", M .. "/Hits/02.mp3" } } })

-- The stick: no stamps and no attributes, only the date FAT keeps.
put("/Drive/Stick/a.mp3", mp3{ title = "Arriba", artist = "Ho Ra", album = "Stick", track = "1" },
    { modified = T0 - 400 * DAY })
put("/Drive/Stick/b.mp3", mp3{ title = "Abajo", artist = "Ho Ra", album = "Stick", track = "2" },
    { modified = T0 - 400 * DAY })

local SONGS = 18

local function paths(rows)
  local out = {}

  for i, s in ipairs(rows or {}) do out[i] = s.path end

  return out
end

local function album_named(lib, title, artist)
  for _, a in ipairs(lib:albums()) do
    if a.title == title and (artist == nil or a.artist == artist) then return a end
  end
end

--------------------------------------------------------------------------
-- The first refresh: everything read, the facts written.
--------------------------------------------------------------------------

local lib = open()
local c = lib:refresh()

check(c.seen == SONGS, "the first refresh sees every song: " .. c.seen)
check(reads == SONGS and c.read == SONGS, "and reads every one: " .. reads)
check(c.unstored == 2, "the stick's two keep nothing: " .. c.unstored)
check(c.failed == 0 and c.removed == 0, "nothing failed or removed")
check(reads_of[M .. "/.hidden/secret.mp3"] == nil, "a hidden folder is not the library's")

do
  local a = fs.getattr(M .. "/Ana Sol/Mar/01 Ola.mp3")
  local n = tree[M .. "/Ana Sol/Mar/01 Ola.mp3"]

  check(a["audio:title"] == "Ola" and a["audio:artist"] == "Ana Sol"
        and a["audio:album"] == "Mar" and a["audio:genre"] == "Pop",
        "the tags are attributes on the file")
  check(a["audio:track"] == 1 and a["audio:year"] == 2019, "numbers as numbers")
  check(a["audio:format"] == "MP3", "the format from the extension, when the reader did not say")
  check(a["audio:read_mtime"] == n.mtime and a["audio:read_size"] == #n.bytes,
        "and the stamp and size it was read at")
  check(a["audio:added"] == OLD, "the day added is the day the disk made it")
  check(a["audio:plays"] == nil, "no play is written before there is one")

  local t = fs.getattr(M .. "/Ana Sol/Tierra/01.flac")

  check(t["audio:rate"] == 96000 and t["audio:bits"] == 24 and t["audio:format"] == "FLAC"
        and t["audio:duration_ms"] == 241500, "a FLAC's rate, depth and length")

  local tr = fs.getattr(M .. "/Ana Sol/Mar/02 Night Fever.mp3")

  check(tr["audio:track"] == 2 and tr["audio:tracks"] == 3, "\"2/3\" is track 2 of 3")
end

--------------------------------------------------------------------------
-- Albums and artists.
--------------------------------------------------------------------------

do
  local albums = lib:albums()

  check(#albums == 8, "eight albums: " .. #albums)

  local mar = album_named(lib, "Mar", "Ana Sol")

  check(mar and mar.count == 3 and not mar.compilation, "Mar, three songs, Ana Sol's")
  check(mar and mar.cover and mar.cover.path == M .. "/Ana Sol/Mar/cover.jpg",
        "a folder's cover.jpg when the songs carry none")
  check(mar and mar.year == 2019 and mar.format == "MP3" and not mar.lossless, "Mar's year and format")
  check(mar and mar.songs[1].title == "Ola" and mar.songs[3].title == "Brisa", "in track order")

  local tierra = album_named(lib, "Tierra", "Ana Sol")

  check(tierra and tierra.count == 2 and tierra.lossless and tierra.rate == 96000
        and tierra.bits == 24 and tierra.duration_ms == 421500,
        "Tierra: lossless, 96/24, its length summed")

  local hits = album_named(lib, "Summer Hits")

  check(hits and hits.count == 3 and hits.compilation and hits.artist == nil,
        "one name, one folder, three artists: one compilation, not three albums")

  local dance = album_named(lib, "Dance 2020")

  check(dance and dance.count == 3 and dance.compilation,
        "Various Artists across CD1 and CD2: one compilation")
  check(dance and dance.songs[1].title == "Alba" and dance.songs[2].title == "Brillo"
        and dance.songs[3].title == "Cielo", "disc one's songs, then disc two's")

  local bo, cy = album_named(lib, "Greatest Hits", "Bo Rey"), album_named(lib, "Greatest Hits", "Cy Lun")

  check(bo and cy and bo ~= cy and bo.count == 2 and cy.count == 2,
        "two albums of one name by two artists stay two")
  check(bo and bo.cover and bo.cover.path == M .. "/Bo Rey/Greatest Hits/02.mp3"
        and bo.cover.offset and bo.cover.bytes == #"\255\216 the cover",
        "a song's own picture, where it is in the file")
  check(cy and cy.lossless and cy.format == "WAV", "Cy Lun's: WAV, lossless")

  local loose = album_named(lib, "Loose")

  check(loose and loose.count == 1 and loose.songs[1].title == "untagged",
        "a song with no album is its folder's, named by its file")

  local stick = album_named(lib, "Stick", "Ho Ra")

  check(stick and stick.count == 2, "the stick's album")
  check(lib:album(mar.id) == mar, "an album found by its id")

  local artists = lib:artists()
  local names = {}

  for i, a in ipairs(artists) do names[i] = a.name end

  same_set(names, { "Ana Sol", "Bo Rey", "Cy Lun", "Di Mar", "Eva Luz", "Fer Sur", "Gil Paz", "Ho Ra" },
           "the artists, and Various Artists is not one")

  local ana = lib:artist("ana sol")

  check(ana and #ana.albums == 2 and ana.albums[1].title == "Mar" and ana.songs == 5,
        "Ana Sol: two albums, five songs")

  local bo_rey = lib:artist("Bo Rey")

  check(bo_rey and #bo_rey.albums == 1 and #bo_rey.appears_on == 1
        and bo_rey.appears_on[1] == hits, "Bo Rey's album, and the compilation he appears on")

  local di = lib:artist("Di Mar")

  check(di and #di.albums == 0 and #di.appears_on == 1, "an artist only on a compilation")
end

do
  local walked = lib:folders()
  local set = {}

  for _, f in ipairs(walked) do set[f] = true end

  check(set[M .. "/Ana Sol/Mar"] and set[M .. "/Dance 2020/CD2"] and set["/Drive/Stick"],
        "every folder listed is one to watch")
  check(not set[M .. "/.hidden"] and not set[M .. "/Playlists"], "and none that is not the library's")
end

--------------------------------------------------------------------------
-- Refreshed again: nothing read that has not changed.
--------------------------------------------------------------------------

reset_reads()
c = lib:refresh()

check(reads == 0 and c.read == 0 and c.kept == SONGS,
      "the same library refreshed reads nothing: " .. reads)

-- What the person did, before a new process looks.
lib:played(M .. "/Ana Sol/Mar/02 Night Fever.mp3", T0 - 3 * DAY)
lib:played(M .. "/Ana Sol/Mar/02 Night Fever.mp3", T0 - 2 * DAY)
lib:played(M .. "/Ana Sol/Mar/02 Night Fever.mp3", T0 - DAY)
lib:played(M .. "/Hits/01.mp3", T0 - 10 * DAY)
lib:played(M .. "/Ana Sol/Tierra/01.flac", T0 - 41 * DAY)
lib:played(M .. "/Ana Sol/Tierra/01.flac", T0 - 40 * DAY)
lib:skipped(M .. "/Hits/02.mp3")
lib:rate(M .. "/Ana Sol/Mar/01 Ola.mp3", 5)
lib:rate(M .. "/Ana Sol/Tierra/02.flac", 4)
lib:rate(M .. "/Ana Sol/Mar/03 Brisa.mp3", 2)
lib:rate(M .. "/Hits/03.mp3", 3)

check(lib:rate(M .. "/Hits/03.mp3", 6) == nil, "six stars is not a rating")
check(fs.getattr(M .. "/Ana Sol/Mar/02 Night Fever.mp3")["audio:plays"] == 3,
      "plays are attributes")

-- A new process: the facts come from the attributes, the stick is read.
reset_reads()

local lib2 = open()

c = lib2:refresh()

check(c.seen == SONGS, "a new process sees every song")
check(reads == 2 and reads_of["/Drive/Stick/a.mp3"] == 1 and reads_of["/Drive/Stick/b.mp3"] == 1,
      "and reads only the stick, which keeps nothing: " .. reads)
check(#lib2:albums() == 8, "the same eight albums, from the attributes")
check(lib2:song(M .. "/Ana Sol/Mar/02 Night Fever.mp3").plays == 3
      and lib2:song(M .. "/Ana Sol/Mar/01 Ola.mp3").rating == 5,
      "plays and ratings come back from the file's attributes")
check(lib2:song(M .. "/Ana Sol/Tierra/01.flac").rate == 96000, "and the rate")

-- One file changed: read again, and only it.
put(M .. "/Ana Sol/Mar/03 Brisa.mp3",
    mp3{ title = "Brisa Nueva", artist = "Ana Sol", album = "Mar", track = "3", genre = "Pop" },
    { created = OLD })
reset_reads()
c = lib2:refresh()

check(reads == 1 and reads_of[M .. "/Ana Sol/Mar/03 Brisa.mp3"] == 1,
      "a file changed is read again, and nothing else: " .. reads)
check(reads_of[M .. "/Ana Sol/Mar/01 Ola.mp3"] == nil, "one unchanged is not")

do
  local s = lib2:song(M .. "/Ana Sol/Mar/03 Brisa.mp3")

  check(s.title == "Brisa Nueva", "its new title")
  check(s.year == nil and fs.getattr(s.path)["audio:year"] == nil,
        "a tag taken out of the file leaves its attribute")
  check(s.rating == 2, "and what the person did is kept across a read")
end

-- The same stamp, another size: read again.
do
  local p = M .. "/Loose/untagged.mp3"
  local m = tree[p].mtime

  put(p, mp3({}, 100), { created = OLD, mtime = m })
  reset_reads()
  lib2:refresh()

  check(reads == 1 and reads_of[p] == 1, "a size that moved is a change too")
end

-- Taken away, and one added.
tree[M .. "/Hits/03.mp3"] = nil
put(M .. "/Hits/04.mp3", mp3{ title = "Cuatro", artist = "Di Mar", album = "Summer Hits", track = "4", year = "2018" },
    { created = OLD })
reset_reads()
c = lib2:refresh()

check(c.removed == 1 and lib2:song(M .. "/Hits/03.mp3") == nil, "a file taken away leaves the library")
check(reads == 1 and reads_of[M .. "/Hits/04.mp3"] == 1, "a file added is read, alone")
check(album_named(lib2, "Summer Hits").count == 3, "and the compilation still has three")

-- Only one folder, as its pulse names it: the rest is left alone.
put(M .. "/Hits/05.mp3", mp3{ title = "Cinco", artist = "Bo Rey", album = "Summer Hits", track = "5", year = "2018" },
    { created = OLD })
reset_reads()
c = lib2:refresh(nil, { M .. "/Hits" })

check(c.folders == 1 and reads == 1 and c.removed == 0,
      "a folder refreshed alone lists one folder and reads its new song")
check(lib2:song(M .. "/Ana Sol/Mar/01 Ola.mp3") ~= nil, "and the songs elsewhere stay")

-- Back as it was for the rules below: 03 again, 04 and 05 away.
tree[M .. "/Hits/04.mp3"], tree[M .. "/Hits/05.mp3"] = nil, nil
put(M .. "/Hits/03.mp3", mp3{ title = "Tres", artist = "Di Mar", album = "Summer Hits", track = "3", year = "2018" },
    { created = OLD })
lib2:refresh()
lib2:rate(M .. "/Hits/03.mp3", 3)

check(#lib2:songs() == SONGS, "eighteen songs again")

-- A slice at a time, as the window does it.
do
  reset_reads()

  local lib3 = open()
  local steps, done, most = 0, false, 0

  lib3:start()

  while not done do
    local before = reads

    done = lib3:step(1)
    steps = steps + 1
    most = math.max(most, reads - before)
  end

  check(steps > 10 and most <= 1, "a refresh in slices of one: " .. steps .. " steps, at most one read each")
  check(#lib3:albums() == 8, "and the same albums at its end")
end

--------------------------------------------------------------------------
-- Long fields: cut, and written a kilobyte at a time.
--------------------------------------------------------------------------

do
  local long = ("é"):rep(300)                    -- 600 bytes

  put("/Home/Long/long.mp3", mp3{ title = long, artist = long, album = long })

  local l = open({ "/Home/Long" })

  l:refresh()

  local a = fs.getattr("/Home/Long/long.mp3")

  check(a["audio:title"] and #a["audio:title"] <= 255 and utf8.len(a["audio:title"]),
        "a long title kept to 255 bytes, cut at a character")
  check((setattr_calls["/Home/Long/long.mp3"] or 0) >= 2 and a["audio:read_mtime"] ~= nil,
        "and the facts written in more than one request, all of them kept")
end

-- A drive that gives neither a stamp nor a date: nothing to compare, so
-- read at every refresh rather than trusted.
do
  put("/Drive/Bare/x.mp3", mp3{ title = "X", artist = "Y", album = "Z" })

  local l = open({ "/Drive/Bare" })

  reset_reads()
  l:refresh()
  l:refresh()

  check(reads == 2, "a file with no stamp and no date is read each time: " .. reads)
  check(lib2:song("/Drive/Stick/a.mp3").added == T0 - 400 * DAY,
        "a stick's song arrived on the date FAT gives it")
end

--------------------------------------------------------------------------
-- Search.
--------------------------------------------------------------------------

do
  local r = lib2:search("night fe")

  same_set(paths(r.songs), { M .. "/Ana Sol/Mar/02 Night Fever.mp3" }, "\"night fe\" finds Night Fever")

  r = lib2:search("GREATEST")

  check(#r.albums == 2 and #r.songs == 4, "\"GREATEST\": two albums, four songs, any case")

  r = lib2:search("ana")

  check(#r.artists == 1 and r.artists[1].name == "Ana Sol" and #r.songs == 5 and #r.albums == 2,
        "\"ana\": the artist, her five songs, her two albums")

  r = lib2:search("various")

  check(#r.songs == 3, "an album artist is searched")

  r = lib2:search("pop ola")

  same_set(paths(r.songs), { M .. "/Ana Sol/Mar/01 Ola.mp3" }, "every word must be there, in any field")

  r = lib2:search("   ")

  check(#r.songs == 0 and #r.albums == 0, "nothing typed finds nothing")
end

--------------------------------------------------------------------------
-- Smart rules, each against the songs it must choose.
--------------------------------------------------------------------------

local ALL = {}

for path, n in pairs(tree) do
  if not n.dir and library.MUSIC[path:match("%.(%w+)$") or ""]
     and not path:find("/.hidden/", 1, true) and not path:find("^/Home/Long")
     and not path:find("^/Drive/Bare") then
    ALL[#ALL + 1] = path
  end
end

check(#ALL == SONGS, "the table of every song")

local function except(list)
  local out, gone = {}, {}

  for _, p in ipairs(list) do gone[p] = true end
  for _, p in ipairs(ALL) do if not gone[p] then out[#out + 1] = p end end

  return out
end

local MAR = { M .. "/Ana Sol/Mar/01 Ola.mp3", M .. "/Ana Sol/Mar/02 Night Fever.mp3",
              M .. "/Ana Sol/Mar/03 Brisa.mp3" }
local TIERRA = { M .. "/Ana Sol/Tierra/01.flac", M .. "/Ana Sol/Tierra/02.flac" }
local HITS = { M .. "/Hits/01.mp3", M .. "/Hits/02.mp3", M .. "/Hits/03.mp3" }
local DANCE = { M .. "/Dance 2020/CD1/01.mp3", M .. "/Dance 2020/CD1/02.mp3",
                M .. "/Dance 2020/CD2/01.mp3" }
local STICK = { "/Drive/Stick/a.mp3", "/Drive/Stick/b.mp3" }

local function join(...)
  local out = {}

  for _, l in ipairs({ ... }) do for _, p in ipairs(l) do out[#out + 1] = p end end

  return out
end

local function rule(name, r, want, ordered)
  local got, why = lib2:smart(r, T0)

  if not got then
    check(false, name .. ": refused, " .. tostring(why))
    return
  end

  if ordered then
    local ok = #got == #want

    for i = 1, #want do ok = ok and got[i] and got[i].path == want[i] end

    check(ok, name .. ", in order - got {" .. table.concat(paths(got), ", ") .. "}")
  else
    same_set(paths(got), want, name)
  end
end

local SMART = {}

for _, s in ipairs(library.SMART) do SMART[s.name] = s.rule end

check(#library.SMART == 5, "five smart playlists ship")

rule("Recently added", SMART["Recently added"], TIERRA)
rule("Most played", SMART["Most played"],
     { M .. "/Ana Sol/Mar/02 Night Fever.mp3", M .. "/Ana Sol/Tierra/01.flac", M .. "/Hits/01.mp3" }, true)
rule("Top rated", SMART["Top rated"],
     { M .. "/Ana Sol/Mar/01 Ola.mp3", M .. "/Ana Sol/Tierra/02.flac" }, true)
rule("Hi-Res", SMART["Hi-Res"], join(TIERRA, { M .. "/Cy Lun/Greatest Hits/02.wav" }))
rule("Never played", SMART["Never played"],
     except({ M .. "/Ana Sol/Mar/02 Night Fever.mp3", M .. "/Ana Sol/Tierra/01.flac", M .. "/Hits/01.mp3" }))

rule("genre is pop, any case", { field = "genre", op = "is", value = "POP" }, MAR)
rule("genre is not pop", { field = "genre", op = "is_not", value = "pop" }, except(MAR))
rule("year between 2018 and 2020", { field = "year", op = "between", value = { 2020, 2018 } },
     join({ MAR[1], MAR[2] }, HITS, DANCE))
rule("year > 2019", { field = "year", op = ">", value = 2019 }, join(TIERRA, DANCE))
rule("format is flac", { field = "format", op = "is", value = "flac" }, TIERRA)
rule("rate >= 88.2 kHz", { field = "rate", op = ">=", value = 88200 }, TIERRA)
rule("bits < 24", { field = "bits", op = "<", value = 24 }, { M .. "/Cy Lun/Greatest Hits/01.wav" })
rule("artist contains sol", { field = "artist", op = "contains", value = "sol" },
     join(MAR, TIERRA))
rule("album lacks hits", { field = "album", op = "lacks", value = "hits" },
     except(join(HITS, { M .. "/Bo Rey/Greatest Hits/01.mp3", M .. "/Bo Rey/Greatest Hits/02.mp3",
                         M .. "/Cy Lun/Greatest Hits/01.wav", M .. "/Cy Lun/Greatest Hits/02.wav" })))
rule("title starts night", { field = "title", op = "starts", value = "night" },
     { M .. "/Ana Sol/Mar/02 Night Fever.mp3" })
rule("compilation is true, marked or not", { field = "compilation", op = "is", value = true },
     join(HITS, DANCE))
rule("played in the last 7 days", { field = "played", op = "in_last", value = 7 },
     { M .. "/Ana Sol/Mar/02 Night Fever.mp3" })
rule("not played in the last 30 days", { field = "played", op = "not_in_last", value = 30 },
     except({ M .. "/Ana Sol/Mar/02 Night Fever.mp3", M .. "/Hits/01.mp3" }))
rule("added more than 30 days ago", { field = "added", op = "<", value = T0 - 30 * DAY },
     except(TIERRA))
rule("skips > 0", { field = "skips", op = ">", value = 0 }, { M .. "/Hits/02.mp3" })
rule("in the playlist Road", { field = "playlist", op = "in", value = "Road" },
     { M .. "/Ana Sol/Mar/01 Ola.mp3", M .. "/Hits/02.mp3" })
rule("not in the playlist Road", { field = "playlist", op = "not_in", value = "Road" },
     except({ M .. "/Ana Sol/Mar/01 Ola.mp3", M .. "/Hits/02.mp3" }))
rule("in a playlist there is not", { field = "playlist", op = "in", value = "Nope" }, {})
rule("any: Bo Rey or Cy Lun",
     { match = "any", rules = { { field = "artist", op = "is", value = "Bo Rey" },
                                { field = "artist", op = "is", value = "Cy Lun" } } },
     { M .. "/Hits/01.mp3", M .. "/Hits/02.mp3", M .. "/Bo Rey/Greatest Hits/01.mp3",
       M .. "/Bo Rey/Greatest Hits/02.mp3", M .. "/Cy Lun/Greatest Hits/01.wav",
       M .. "/Cy Lun/Greatest Hits/02.wav" })
rule("all: Ana Sol and rated",
     { match = "all", rules = { { field = "artist", op = "is", value = "ana sol" },
                                { field = "rating", op = ">", value = 0 } } },
     { M .. "/Ana Sol/Mar/01 Ola.mp3", M .. "/Ana Sol/Mar/03 Brisa.mp3", M .. "/Ana Sol/Tierra/02.flac" })
rule("lossless, two highest rates",
     { match = "all", rules = { { field = "lossless", op = "is", value = true } },
       by = "rate", descending = true, limit = 2 }, TIERRA, true)
rule("all of nothing is everything", { match = "all", rules = {} }, ALL)
rule("any of nothing is none", { match = "any", rules = {} }, {})

do
  local bad = {
    { { field = "colour", op = "is", value = "red" }, "colour" },
    { { field = "genre", op = ">", value = 3 }, "genre" },
    { { field = "year", op = "between", value = { 2018 } }, "between" },
    { { field = "rate", op = ">=", value = "fast" }, "number" },
    { { field = "lossless", op = "is", value = "yes" }, "true or false" },
    { { match = "some", rules = {} }, "all or any" },
    { { match = "all", rules = {}, limit = 0 }, "limit" },
    { { match = "all", rules = {}, by = "mood" }, "mood" },
  }

  for _, b in ipairs(bad) do
    local got, why = lib2:smart(b[1], T0)

    check(got == nil and tostring(why):find(b[2], 1, true),
          "a rule that is not one is refused, naming it: " .. tostring(why))
  end
end

do
  local page, total = lib2:songs{ where = SMART["Never played"], sort = "title", from = 2, count = 3 }

  check(total == SONGS - 3 and #page == 3, "songs, where a rule says, a page at a time")
  check(page[1].title <= page[2].title and page[2].title <= page[3].title, "sorted by title")

  local all = lib2:songs{ sort = "plays", descending = true }

  check(all[1].path == M .. "/Ana Sol/Mar/02 Night Fever.mp3", "the column sort, most played first")
end

print(("library: %d passed, %d failed"):format(passed, failed))

os.exit(failed == 0 and 0 or 1)
