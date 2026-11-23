-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: application
-- kosmos: icon App_MediaPlayer
-- kosmos: name Music
-- kosmos: section applications
-- kosmos: opens mp3 flac m4a wav aif aiff ogg oga opus
-- kosmos: needs audio
-- kosmos: image media
-- kosmos: depends media
-- kosmos: about Your music as a library, played at each file's own resolution
--
-- Music: a library player on iTunes's ground, at the file's own resolution
-- (`docs/player.md`, step P5; drawn in `docs/player.html`).
--
--   wm music                                  the library, /Home/Music
--   wm music:/Home/Music/Album/01-Song.flac   that song, its album next
--   wm music:/Home/Elsewhere                  that folder in the library too
--
-- **The next Music, not a second player** (decision 1): it replaces the
-- VOX pilot of 14 September, and keeps what that one agreed - the window in
-- the system's own look, what is playing on top, Songs, Albums, Artists.
--
-- **The thinnest layer** (`CLAUDE.md`, *Kits supply, applications
-- orchestrate*). Nothing in this file reads a file's bytes or a sample:
--
--   - the library - songs, albums, compilations, artists, search, the five
--     smart playlists, plays - is the Library kit (`library.lua`, P4), its
--     facts read by `tags.lua` and the Media Kit through `playqueue.facts`;
--   - playing is the play queue (`playqueue.lua`): the Media Kit decodes
--     untrimmed, the Playback Kit trims, joins and plays on its thread in
--     the audio band (P1, P3);
--   - **what reached the speaker is the audio server's word**, `exact` and
--     `why` per stream (`audio.streams()`), never this window's claim;
--   - a cover is the kit's thumbnail of the picture inside the file
--     (`pictures.part`, `thumb:WxH:`), decoded here as it comes into view,
--     one a pass, small.
--
-- **Nothing waits on a server in a paint** (`CLAUDE.md`). The library is
-- refreshed a few files a pass, the queue fed a few milliseconds a pass,
-- the server asked what it did four times a second - all on the window's
-- clock, in `on_frame` - and a paint draws what was last heard. A control
-- paints its new state at the click, from what this window already knows.
--
-- **Left for later steps**, as `player.md` orders them: the signal path,
-- Up Next, the column browser, playlists made and edited, ratings set, the
-- mini player and the replicant (P8, P9); the equaliser and the L/R meter
-- (P10 - the one meter here is the server's peak of the stream, which is
-- one number); ReplayGain and crossfade (P11); Info, radio and the output
-- chooser (P12). The window draws none of their controls until they do
-- something: a control that does nothing is a lie on the screen.

local ui        = use("/Kosmos/Libraries/ui.lua")
local files     = use("/Kosmos/Libraries/files.lua")
local audio     = use("/Kosmos/Libraries/audio.lua")
local clock     = use("/Kosmos/Libraries/clock.lua")
local library   = use("/Kosmos/Libraries/library.lua")
local playqueue = use("/Kosmos/Libraries/playqueue.lua")
local pictures  = use("/Kosmos/Libraries/pictures.lua")
local theme = ui.theme
local L = ui.layout

--------------------------------------------------------------------------
-- Where things are, as `player.html` draws them.
--------------------------------------------------------------------------

local W, H = 1080, 660
local SIDE = 206                -- the sidebar's column
local NOW_H = 96                -- what is playing, under the header
local TOP = L.head + NOW_H      -- where the sidebar and the main view begin
local FOOT = 30                 -- the sidebar's foot
local ROW = 31                  -- a song's row
local HEAD_ROW = 30             -- the columns' names
local CARD = 150                -- an album's cover in a grid
local BIG = 240                 -- an album's cover, opened
local GAP = 18                  -- between two cards
local FACE = 28                 -- an artist's face

--------------------------------------------------------------------------
-- What it was asked for.
--------------------------------------------------------------------------

local MUSIC = "/Home/Music"
local roots = { MUSIC }
local given                      -- a song to start with, its album next

for _, word in ipairs(files.words(args or "")) do
  if word:sub(1, 1) == "/" then
    local attrs = fs.getattr(word)

    if attrs and attrs.kind == "directory" then
      if word ~= MUSIC then roots[#roots + 1] = (word:gsub("/$", "")) end
    elseif attrs then
      given = word

      local folder = files.parent(word)
      local inside = false

      for _, r in ipairs(roots) do
        if folder == r or folder:sub(1, #r + 1) == r .. "/" then inside = true end
      end

      if not inside then roots[#roots + 1] = folder end
    else
      print("music: there is nothing at " .. word)
    end
  end
end

local win, err = ui.window{ title = "Music", w = W, h = H, x = 140, y = 70,
                            header = true }

if not win then
  print("music: " .. tostring(err))
  return
end

W, H = win.root.w, win.root.h

local hz = (fs.read("/Devices/cpu") or {}).counter_hz or 62500000

local function epoch() return (clock.now() or {}).epoch end

--------------------------------------------------------------------------
-- The library, and what is shown of it.
--------------------------------------------------------------------------

local lib = library.open{ folders = roots, tags_of = playqueue.facts, now = epoch }

local view = "songs"             -- songs, albums, album, artists, search
local source = "songs"           -- the sidebar's choice the songs come from
local sort, descending = "artist", false
local album_sort = "artist"
local rows = {}                  -- the songs shown, in order
local albums, artists = {}, {}
local opened                     -- the album opened
local artist_on                  -- the artist chosen
local found                      -- search's results
local query = ""
local selected = 1               -- a row of `rows`
local top = 0                    -- pixels scrolled, the main view's
local said = ""                  -- a sentence for the foot when something went wrong
local refreshing = false
local began_refresh = 0

-- Songs by field: the sidebar's smart playlists are `library.SMART`'s.
local SMART = {}

for _, s in ipairs(library.SMART) do SMART[s.name] = s.rule end

local function songs_of(choice)
  if choice == "songs" then
    return (lib:songs{ sort = sort, descending = descending })
  end

  local name = choice:match("^smart:(.*)$")

  if name and SMART[name] then
    local rule = {}

    for k, v in pairs(SMART[name]) do rule[k] = v end

    -- A column chosen wins over the rule's own order.
    if sort ~= "artist" or descending then
      rule.by, rule.descending = sort, descending
    end

    return lib:smart(rule) or {}
  end

  local list = choice:match("^playlist:(.*)$")

  if list then
    local out = {}

    for _, path in ipairs(lib:playlist(list) or {}) do
      local s = lib:song(path)

      if s then out[#out + 1] = s end
    end

    return out
  end

  local folder = choice:match("^folder:(.*)$")

  if folder then
    local out = {}

    for _, s in ipairs((lib:songs{ sort = sort, descending = descending })) do
      if s.path:sub(1, #folder + 1) == folder .. "/" then out[#out + 1] = s end
    end

    return out
  end

  return {}
end

local ALBUM_SORTS = {
  artist = function(x, y)
    local ax, ay = (x.artist or "~"):lower(), (y.artist or "~"):lower()

    if ax ~= ay then return ax < ay end
    return x.title:lower() < y.title:lower()
  end,
  title = function(x, y) return x.title:lower() < y.title:lower() end,
  year = function(x, y)
    if (x.year or 0) ~= (y.year or 0) then return (x.year or 0) > (y.year or 0) end
    return x.title:lower() < y.title:lower()
  end,
  added = function(x, y)
    if (x.added or 0) ~= (y.added or 0) then return (x.added or 0) > (y.added or 0) end
    return x.title:lower() < y.title:lower()
  end,
}

local relayout                   -- below: what moves with the view and the size
local place_buttons

-- Everything shown, from the library as it is now.
local function reshow()
  rows = songs_of(source)
  albums = {}

  for i, a in ipairs(lib:albums()) do albums[i] = a end

  table.sort(albums, ALBUM_SORTS[album_sort])
  artists = lib:artists()

  if opened then opened = lib:album(opened.id) or opened end

  if query ~= "" then found = lib:search(query) end

  selected = math.max(1, math.min(selected, #rows))

  if relayout then relayout() end

  win.dirty = true
end

--------------------------------------------------------------------------
-- Words.
--------------------------------------------------------------------------

local function khz(rate)
  if not rate or rate == 0 then return "?" end
  if rate % 1000 == 0 then return tostring(rate // 1000) end

  return ("%.1f"):format(rate / 1000)
end

local function kbps_of(s)
  local src = s.source

  if src and (src.bitrate or 0) > 0 then return src.bitrate // 1000 end

  if (s.size or 0) > 0 and (s.duration_ms or 0) > 0 then
    return math.floor(s.size * 8 / s.duration_ms + 0.5)
  end

  return nil
end

-- "96 kHz · 24", or "320 kbps": the Quality column's words.
local function quality(s)
  if s.lossless and s.rate then
    return s.bits and ("%s kHz \u{00b7} %d"):format(khz(s.rate), s.bits)
           or ("%s kHz"):format(khz(s.rate))
  end

  local k = kbps_of(s)

  return k and ("%d kbps"):format(k) or ""
end

local function hi_res(x)
  return x.lossless and ((x.rate or 0) >= 88200 or (x.bits or 0) >= 24)
end

local function minutes(ms)
  return clock.duration((ms or 0) / 1000)
end

local function count(n, one, many)
  return ("%d %s"):format(n, n == 1 and one or (many or one .. "s"))
end

-- A long length as a person says it: "47 min", "2 h 51 min", "6.4 days".
local function length_words(ms)
  local s = (ms or 0) // 1000

  if s >= 86400 then return ("%.1f days"):format(s / 86400) end
  if s >= 3600 then return ("%d h %d min"):format(s // 3600, s % 3600 // 60) end

  return ("%d min"):format(s // 60)
end

local function initials(name)
  local out = {}

  for w in tostring(name or "?"):gmatch("[%w\128-\255]+") do
    out[#out + 1] = w:sub(1, 1):upper()
    if #out == 2 then break end
  end

  return #out > 0 and table.concat(out) or "?"
end

-- A colour of its own for a name: an album without a picture, a face.
local SWATCHES = { 0xff3d4f9a, 0xff6f9a8d, 0xff5b4b8a, 0xffc94f4f, 0xffc8622d,
                   0xff155a6d, 0xff7a5fc0, 0xff3b5d3a, 0xff555b6a, 0xff8a6514 }

local function swatch(name)
  local h = 0

  for i = 1, #tostring(name or "") do h = (h * 31 + tostring(name):byte(i)) % 1000003 end

  return SWATCHES[h % #SWATCHES + 1]
end

--------------------------------------------------------------------------
-- Covers: the kit's thumbnails, decoded in this process as they come into
-- view - one a pass, through `win:soon` - and drawn as lent pictures.
--------------------------------------------------------------------------

local ready = {}                 -- a thumbnail's name: true, or false if it never will be
local ready_order = {}
local asked = {}                 -- names waiting for a pass
local READY_MOST = 48            -- under the kit's 64, so none is dropped behind our back

-- The picture an album's cover is: the one inside a song, or the folder's.
local function cover_source(a)
  local c = a and a.cover

  if not c then return nil end

  if c.offset and c.bytes then return pictures.part(c.path, c.offset, c.bytes, c.mime) end

  return c.path
end

local function thumb_name(a, size)
  local src = cover_source(a)

  return src and ("thumb:%dx%d:%s"):format(size, size, src) or nil
end

local function decode_next()
  local name = table.remove(asked, 1)

  if not name then return end

  ready[name] = ui.picture(name) and true or false
  ready_order[#ready_order + 1] = name

  -- The oldest forgotten here before the kit forgets it, so a cover it
  -- let go is asked for again rather than decoded inside a paint.
  while #ready_order > READY_MOST do
    ready[table.remove(ready_order, 1)] = nil
  end

  win.dirty = true

  if #asked > 0 then win:soon(decode_next) end
end

local function want(name)
  if not name or ready[name] ~= nil or asked[name] then return end

  asked[name] = true
  asked[#asked + 1] = name

  if #asked == 1 then win:soon(function() asked[name] = nil decode_next() end) end
end

--
-- **A cover at `x, y`, `size` square**: the picture when it has been
-- decoded, and asked for when it has not; until then - and for an album
-- with none - a square of the album's own colour with its initials, as the
-- drawing has it.
--
local function draw_cover(g, a, x, y, size, thumb)
  thumb = thumb or (size > CARD and BIG or CARD)

  local name = thumb_name(a, thumb)

  if name and ready[name] then
    g:lent(x, y, size, size, name, thumb, thumb)
    return
  end

  want(name)

  local title = a and a.title or "?"
  local r = size >= 100 and 8 or 4

  g:fill_round(x, y, size, size, swatch(title), r)

  local mark = initials(title)
  local role = size >= 100 and "title" or "ui"

  g:text(x + (size - gfx.measure(mark, role)) // 2, y + (size - gfx.height(role)) // 2,
         mark, 0xffffffff, nil, role)
end

--------------------------------------------------------------------------
-- What is playing.
--------------------------------------------------------------------------

local q, qwhy = playqueue.open{ name = "Music" }

if not q then print("music: " .. tostring(qwhy)) end

local now = {}                   -- `q:now()`, as of the last pass
local verdict                    -- the server's word on the stream: { exact, why }
local device_name = ""
local peak = 0
local level = 1.0                -- the volume, 0 to 1
local badge_said = nil

--
-- **The badge**: the song's format in its own words, and what reached the
-- speaker as the audio server says it - "bit-perfect" for a lossless song
-- nothing changed, "decoded, nothing changed after" for a lossy one, which
-- is never bit-perfect to its source (`player.html`), "converted" and why
-- otherwise; and nothing at all while the server has not said.
--
local function badge_of(src)
  if not src then return nil, nil end

  local words

  if src.lossless and src.rate then
    words = src.bits and ("%s %s kHz / %d-bit"):format(src.format or "PCM", khz(src.rate), src.bits)
            or ("%s %s kHz"):format(src.format or "PCM", khz(src.rate))
  else
    local k = (src.bitrate or 0) > 0 and src.bitrate // 1000 or nil

    words = k and ("%s %d kbps"):format(src.format or "?", k)
            or ("%s %s kHz"):format(src.format or "?", khz(src.rate))
  end

  if not verdict then return words, nil end

  if verdict.exact then
    return words .. (src.lossless and " \u{00b7} bit-perfect"
                     or " \u{00b7} decoded, nothing changed after"), true
  end

  return words .. " \u{00b7} converted (" .. (verdict.why ~= "" and verdict.why or "the server says")
         .. ")", false
end

local function song_title(s)
  return type(s) == "table" and (s.title or (s.path or ""):match("[^/]*$"))
         or tostring(s or "")
end

local function is_playing(s)
  return now.song and s and now.song.path == s.path
end

--
-- **The queue, given**: the songs in the order shown, from the one chosen.
-- The window says what it did at the click; what is heard follows.
--
local function play_list(list, from)
  if not q then
    said = tostring(qwhy)
    win.dirty = true
    return
  end

  if #list == 0 then return end

  local ok, why = q:set(list, from or 1)

  if not ok and why then said = tostring(why) end

  now = q:now()
  verdict = nil
  print(("music: playing %d of %d, %s"):format(from or 1, #list, song_title(list[from or 1])))
  win.dirty = true
end

-- A play counted when a song played to its end (`player.md`, as iTunes counts).
if q then
  function q.on_heard(_, index, was, natural)
    local was_song = was and q.list[was]

    if natural and was_song and was_song.path then
      lib:played(was_song.path, epoch())
    end

    local song = index and q.list[index]

    if song then
      print(("music: heard %s"):format(song_title(song)))
      verdict = nil
    else
      print("music: the queue ended")
    end

    win.dirty = true
  end
end

--------------------------------------------------------------------------
-- The sidebar's column: its ground, the title and the foot.
--------------------------------------------------------------------------

local side_ground = ui.view{ x = 0, y = 0, w = SIDE, h = H,
                             follow = { "left", "top", "bottom" } }

function side_ground:draw(g)
  g:fill(0, 0, self.w, self.h, "panel")
  g:fill(self.w - 1, 0, 1, self.h, theme.line_soft)
  g:fill(0, L.head - 1, self.w, 1, theme.line_soft)
  g:text(L.head_in, (L.head - 1 - gfx.height("title")) // 2, "Music", theme.text, nil, "title")

  local total, bytes = 0, 0

  for _, s in pairs(lib.by_path) do
    total = total + (s.duration_ms or 0)
    bytes = bytes + (s.size or 0)
  end

  local n = 0

  for _ in pairs(lib.by_path) do n = n + 1 end

  local foot = refreshing and ("reading the library\u{2026} %d songs"):format(n)
               or ("%s \u{00b7} %s \u{00b7} %.1f GB"):format(count(n, "song"),
                                                         length_words(total), bytes / 1e9)

  g:fill(0, self.h - FOOT, self.w, 1, theme.line_soft)
  g:text(L.head_in, self.h - FOOT + (FOOT - gfx.height("ui")) // 2,
         ui.fitted(said ~= "" and said or foot, self.w - 2 * L.head_in, "ui"),
         theme.text_dim, nil, "ui")
end

local side

local function sidebar_items()
  local n_songs = 0

  for _ in pairs(lib.by_path) do n_songs = n_songs + 1 end

  local items = {
    { heading = true, name = "LIBRARY" },
    { id = "songs", name = "Songs", icon = "music", note = tostring(n_songs) },
    { id = "albums", name = "Albums", icon = "folder", note = tostring(#albums) },
    { id = "artists", name = "Artists", icon = "comment", note = tostring(#artists) },
    { id = "smart:Recently added", name = "Recently added", icon = "recent" },
    { gap = true },
    { heading = true, name = "PLAYLISTS" },
  }

  for _, s in ipairs(library.SMART) do
    if s.name ~= "Recently added" then
      items[#items + 1] = { id = "smart:" .. s.name, name = s.name, icon = "star" }
    end
  end

  for _, name in ipairs(lib:playlists()) do
    items[#items + 1] = { id = "playlist:" .. name, name = name, icon = "document" }
  end

  items[#items + 1] = { gap = true }
  items[#items + 1] = { heading = true, name = "PLACES" }

  for _, r in ipairs(roots) do
    items[#items + 1] = { id = "folder:" .. r, name = r, icon = "folder" }
  end

  return items
end

--------------------------------------------------------------------------
-- The header: the view's name and its count, the three views, the search.
--------------------------------------------------------------------------

local search = ui.field{ w = 240, text = "", hint = "Search the library", name = "Search" }
local views = ui.segments{ items = { "Songs", "Albums", "Artists" }, on = 1 }
local header = ui.header{ x = SIDE, y = 0, w = W - SIDE, title = "Songs",
                          title_bar = true, right = { views, search } }

--------------------------------------------------------------------------
-- What is playing, across the top.
--------------------------------------------------------------------------

local nowv = ui.view{ x = 0, y = L.head, w = W, h = NOW_H, follow = { "left", "right", "top" } }

-- The transport's places, in the band, laid out by `place_now`.
local spots = {}
local TOOLS_W = 300

local function place_now(w)
  local x = 16 + 68 + 14
  local ty = 52

  spots.shuffle = { x = x, y = ty + 6, w = 20, h = 20 }
  spots.prev = { x = x + 34, y = ty + 6, w = 20, h = 20 }
  spots.play = { x = x + 66, y = ty, w = 32, h = 32 }
  spots.next = { x = x + 110, y = ty + 6, w = 20, h = 20 }
  spots["repeat"] = { x = x + 144, y = ty + 6, w = 20, h = 20 }

  local bx = x + 190
  local bw = math.max(80, w - TOOLS_W - bx - 60)

  spots.bar = { x = bx + 44, y = ty + 12, w = bw - 88, h = 8 }
  spots.vol = { x = w - TOOLS_W + 60, y = 62, w = 110, h = 8 }
end

local function arrow(g, x, y, s, colour, back)
  if back then
    g:triangle(x + s, y, x + s, y + s, x, y + s / 2, colour)
  else
    g:triangle(x, y, x, y + s, x + s, y + s / 2, colour)
  end
end

local function draw_skip(g, r, colour, back)
  local s = 14
  local x, y = r.x + (r.w - s) // 2, r.y + (r.h - s) // 2
  local bar = 2

  if back then
    g:fill(x, y, bar, s, colour)
    arrow(g, x + bar + 1, y, s - bar - 1, colour, true)
  else
    arrow(g, x, y, s - bar - 1, colour)
    g:fill(x + s - bar, y, bar, s, colour)
  end
end

local function draw_shuffle(g, r, colour)
  local x, y, s, t = r.x + 3, r.y + 3, 14, 2

  g:fill(x, y + t, s - t * 3, t, colour)
  g:fill(x, y + s - t * 2, s - t * 3, t, colour)
  arrow(g, x + s - t * 3, y, t * 3, colour)
  arrow(g, x + s - t * 3, y + s - t * 3, t * 3, colour)
end

local function draw_repeat(g, r, colour, one)
  local x, y, s, t = r.x + 3, r.y + 4, 14, 2

  g:fill(x, y, s - t * 2, t, colour)
  g:fill(x, y, t, s // 2, colour)
  g:fill(x + t * 2, y + s - t, s - t * 2, t, colour)
  g:fill(x + s - t, y + s // 2, t, s // 2, colour)
  arrow(g, x + s - t * 3, y - t, t * 3, colour)

  if one then g:text(x + s + 1, y + s - gfx.height("ui") + 3, "1", colour, nil, "ui") end
end

function nowv:draw(g)
  g:fill(0, 0, self.w, self.h, theme.raised)
  g:fill(0, self.h - 1, self.w, 1, theme.line_soft)

  local song = now.song
  local s = song and lib:song(song.path) or song
  local a = s and s.album_id and lib:album(s.album_id)

  if s then
    draw_cover(g, a or { title = s.album or s.title }, 16, 14, 68, CARD)
  else
    g:fill_round(16, 14, 68, 68, theme.sunken, 6)
    g:line_icon(16 + 26, 14 + 26, "music", theme.text_dim)
  end

  local x = 16 + 68 + 14
  local room = self.w - TOOLS_W - x - 16

  if s then
    local title = ui.fitted(s.title or "", math.min(room, 320), "text")

    g:text(x, 14, title, theme.text, nil, "text")

    local tx = x + gfx.measure(title, "text") + 10
    local meta = (s.artist or "unknown artist") .. (s.album and (" \u{2014} " .. s.album) or "")
    local badge, exact = badge_of(now.source)
    local bw = badge and gfx.measure(badge, "ui") + 14 or 0
    local mroom = x + room - tx - bw - 10

    if mroom > 40 then
      meta = ui.fitted(meta, mroom, "ui")
      g:text(tx, 16, meta, theme.text_dim, nil, "ui")
      tx = tx + gfx.measure(meta, "ui") + 10
    end

    if badge then
      local colour = exact == true and theme.good or exact == false and theme.io_out
                     or theme.text_dim

      g:frame_round(tx, 13, bw, 20, colour, 4)
      g:text(tx + 7, 13 + (20 - gfx.height("ui")) // 2, badge, colour, nil, "ui")
    end
  else
    g:text(x, 14, q and "Nothing playing" or tostring(qwhy), theme.text_dim, nil, "text")
  end

  -- The transport, its new state the moment it was pressed.
  local playing = q and q.playing
  local on = theme.accent
  local idle = theme.text_dim

  draw_shuffle(g, spots.shuffle, q and q.shuffled and on or idle)
  draw_skip(g, spots.prev, theme.text, true)

  local p = spots.play

  g:fill_round(p.x, p.y, p.w, p.h, theme.text, p.w // 2)

  if playing then
    g:fill(p.x + 10, p.y + 9, 4, 14, theme.raised)
    g:fill(p.x + 18, p.y + 9, 4, 14, theme.raised)
  else
    arrow(g, p.x + 12, p.y + 9, 14, theme.raised)
  end

  draw_skip(g, spots.next, theme.text)

  local mode = q and q.repeat_mode or "off"

  draw_repeat(g, spots["repeat"], mode ~= "off" and on or idle, mode == "one")

  -- The bar, and the times either side of it.
  local b = spots.bar
  local at, length = now.at or 0, now.length or 0
  local frac = length > 0 and math.min(1, at / length) or 0

  g:text(b.x - 44, b.y - 3, clock.duration(at), theme.text_dim, nil, "ui")
  g:text(b.x + b.w + 10, b.y - 3, clock.duration(length), theme.text_dim, nil, "ui")
  g:fill_round(b.x, b.y + 2, b.w, 4, theme.track, 2)
  g:fill_round(b.x, b.y + 2, math.floor(b.w * frac), 4, theme.text, 2)

  if s then g:fill_round(b.x + math.floor(b.w * frac) - 6, b.y - 2, 12, 12, theme.text, 6) end

  -- The meter: the server's peak of this stream, one number - the L/R
  -- meter is P10's, in the server.
  local mx, my = self.w - TOOLS_W + 20, 22
  local lamps = math.floor(math.min(1, peak) * 20 + 0.5)

  g:text(mx, my - 3, "level", theme.text_dim, nil, "ui")

  for i = 1, 20 do
    local colour = theme.track

    if i <= lamps then
      colour = i <= 14 and theme.good or i <= 18 and 0xffffcc33 or theme.bad
    end

    g:fill(mx + 40 + (i - 1) * 7, my, 5, 8, colour)
  end

  -- The volume: this stream's gain at the server, full being unchanged.
  local v = spots.vol

  g:line_icon(v.x - 24, v.y - 4, level > 0 and "sound" or "muted", theme.text_dim)
  g:fill_round(v.x, v.y, v.w, 4, theme.track, 2)
  g:fill_round(v.x, v.y, math.floor(v.w * level), 4, theme.text, 2)
  g:fill_round(v.x + math.floor(v.w * level) - 6, v.y - 4, 12, 12, theme.text, 6)

  if device_name ~= "" then
    local d = ui.fitted(device_name, TOOLS_W - 30, "ui")

    g:text(self.w - 16 - gfx.measure(d, "ui"), 76, d, theme.text_dim, nil, "ui")
  end
end

local function inside(r, x, y)
  return r and x >= r.x - 4 and x < r.x + r.w + 4 and y >= r.y - 6 and y < r.y + r.h + 6
end

local dragging

local function seek_to(x)
  local b = spots.bar
  local frac = math.max(0, math.min(1, (x - b.x) / b.w))

  if q and now.song and (now.length or 0) > 0 then
    now.at = frac * now.length         -- the knob there at once
    q:seek(frac * now.length)
  end
end

local function volume_to(x)
  local v = spots.vol

  level = math.max(0, math.min(1, (x - v.x) / v.w))

  if q then q:volume(level) end
end

function nowv:mouse(action, x, y)
  if action == "press" then
    if inside(spots.bar, x, y) then
      dragging = "bar"
      seek_to(x)
      win.dirty = true
      return true
    elseif inside(spots.vol, x, y) then
      dragging = "vol"
      volume_to(x)
      win.dirty = true
      return true
    end
  elseif action == "move" and dragging == "vol" then
    volume_to(x)
    win.dirty = true
    return true
  elseif action == "release" and dragging then
    if dragging == "bar" then seek_to(x) end
    dragging = nil
    win.dirty = true
    return true
  end

  return ui.click(self, action, x, y, function(px, py)
    for name, r in pairs(spots) do
      if name ~= "bar" and name ~= "vol" and inside(r, px, py) then
        return name, function()
          if not q then return end

          if name == "play" then
            if now.song then q:toggle() else play_list(rows, selected) end
          elseif name == "next" then
            q:next()
          elseif name == "prev" then
            q:previous()
          elseif name == "shuffle" then
            q:shuffle(not q.shuffled)
          elseif name == "repeat" then
            q:repeating(({ off = "all", all = "one", one = "off" })[q.repeat_mode])
          end

          now = q:now()
          win.dirty = true
        end
      end
    end

    return nil
  end)
end

--------------------------------------------------------------------------
-- The main view: Songs, Albums, an album opened, Artists, and search.
--------------------------------------------------------------------------

local main = ui.view{ x = SIDE, y = TOP, w = W - SIDE, h = H - TOP,
                      follow = { "left", "right", "top", "bottom" } }
main.focusable = true

local play_btn = ui.button{ text = "\u{25b6} Play", go = true, hidden = true }
local shuffle_btn = ui.button{ text = "Shuffle", hidden = true }

-- The columns of the song list, and how wide each is (0: what is left).
local COLUMNS = {
  { key = "track", name = "", w = 40, right = true },
  { key = "title", name = "Title", w = 0 },
  { key = "artist", name = "Artist", w = 170 },
  { key = "album", name = "Album", w = 170 },
  { key = "duration_ms", name = "Time", w = 54, right = true },
  { key = "format", name = "Format", w = 66 },
  { key = "rate", name = "Quality", w = 110 },
  { key = "rating", name = "Rating", w = 70 },
  { key = "plays", name = "Plays", w = 50, right = true },
}

local function column_widths(w)
  local fixed = 0

  for _, c in ipairs(COLUMNS) do fixed = fixed + c.w end

  local out, x = {}, 0

  for i, c in ipairs(COLUMNS) do
    local cw = c.w > 0 and c.w or math.max(120, w - fixed - 20)

    out[i] = { x = x + 10, w = cw - 10 }
    x = x + cw
  end

  return out
end

local function format_chip(g, x, y, s)
  if not s.format then return end

  local colour = hi_res(s) and theme.good or s.lossless and theme.ring or theme.text_dim
  local w = gfx.measure(s.format, "ui") + 10

  g:frame_round(x, y, w, 18, colour, 4)
  g:text(x + 5, y + (18 - gfx.height("ui")) // 2, s.format, colour, nil, "ui")
end

local function eq_bars(g, x, y)
  g:fill(x, y + 5, 3, 6, theme.accent)
  g:fill(x + 5, y, 3, 11, theme.accent)
  g:fill(x + 10, y + 3, 3, 8, theme.accent)
end

local function draw_stars(g, x, y, n)
  for i = 1, n or 0 do g:line_icon(x + (i - 1) * 13, y, "starred", theme.io_out, 15) end
end

--
-- A list of songs from `y`, every column or the short ones; `list` and the
-- row chosen. Returns the height drawn.
--
local function draw_songs(g, list, y, w, chosen, short, numbered)
  local cols = column_widths(w)
  local th = gfx.height("ui")

  local function cell(i, text, colour, ry)
    local c, col = cols[i], COLUMNS[i]
    local t = ui.fitted(text or "", c.w - 6, "ui")
    local tx = col.right and (c.x + c.w - 6 - gfx.measure(t, "ui")) or c.x

    g:text(tx, ry + (ROW - th) // 2, t, colour, nil, "ui")
  end

  if not short then
    for i, col in ipairs(COLUMNS) do
      local on = col.key == sort
      local name = col.name .. (on and (descending and " \u{25be}" or " \u{25b4}") or "")

      cell(i, name, on and theme.text or theme.text_dim, y)
    end

    g:fill(0, y + HEAD_ROW - 1, w, 1, theme.line_soft)
    y = y + HEAD_ROW
  end

  local first_y = y
  local last = short and #list or #list

  for i = 1, last do
    local ry = first_y + (i - 1) * ROW

    if ry + ROW >= 0 and ry < main.h + top then
      local s = list[i]
      local playing = is_playing(s)
      local on = i == chosen
      local ink = on and theme.text_on or theme.text
      local dim = on and theme.text_on or theme.text_dim

      if on then
        g:fill(0, ry, w, ROW, theme.accent)
      elseif playing then
        g:fill(0, ry, w, ROW, theme.mix(theme.window, theme.accent, 60))
      elseif i % 2 == 0 then
        g:fill(0, ry, w, ROW, theme.mix(theme.window, theme.text, 5))
      end

      if playing then
        eq_bars(g, cols[1].x + cols[1].w - 20, ry + 10)
      else
        cell(1, tostring(numbered and i or (s.track or "")), dim, ry)
      end

      cell(2, s.title, ink, ry)
      cell(3, s.artist or "", dim, ry)
      cell(4, s.album or "", dim, ry)
      cell(5, s.duration_ms and minutes(s.duration_ms) or "", dim, ry)
      format_chip(g, cols[6].x, ry + (ROW - 18) // 2, s)
      cell(7, quality(s), dim, ry)
      draw_stars(g, cols[8].x, ry + (ROW - 15) // 2, s.rating)
      cell(9, tostring(s.plays or 0), dim, ry)
    end
  end

  return (short and 0 or HEAD_ROW) + #list * ROW
end

-- How many cards a row holds in `w`.
local function per_row(w, size)
  return math.max(1, (w - 40 + GAP) // ((size or CARD) + GAP))
end

local function card_h(size) return (size or CARD) + 8 + 2 * gfx.height("ui") + GAP end

local function draw_cards(g, list, y, w, size, sub)
  size = size or CARD

  local n = per_row(w, size)

  for i, a in ipairs(list) do
    local col, row = (i - 1) % n, (i - 1) // n
    local x = 20 + col * (size + GAP)
    local cy = y + row * card_h(size)

    if cy + card_h(size) >= top and cy < top + main.h then
      draw_cover(g, a, x, cy, size, CARD)

      if hi_res(a) then
        local mark = ("%s/%d"):format(khz(a.rate), a.bits or 16)
        local mw = gfx.measure(mark, "ui") + 8

        g:fill_round(x + size - mw - 6, cy + size - 22, mw, 16, 0xc80f141c, 3)
        g:text(x + size - mw - 2, cy + size - 22 + (16 - gfx.height("ui")) // 2, mark,
               theme.good, nil, "ui")
      end

      if now.song and opened ~= a and a.id and now.song.album_id == a.id then
        g:frame_round(x - 3, cy - 3, size + 6, size + 6, theme.accent, 9)
      end

      g:text(x, cy + size + 6, ui.fitted(a.title or "", size, "ui"), theme.text, nil, "ui")
      g:text(x, cy + size + 6 + gfx.height("ui"),
             ui.fitted(sub and sub(a) or "", size, "ui"), theme.text_dim, nil, "ui")
    end
  end

  return ((#list + n - 1) // n) * card_h(size)
end

local function album_sub(a)
  local who = a.compilation and "Various artists" or a.artist or "unknown artist"

  return a.year and (who .. " \u{00b7} " .. a.year) or who
end

-- The album opened's facts line: "2021 · 2 discs · 11 songs · 47 min · FLAC 96 kHz / 24-bit".
local function album_facts(a)
  local parts = {}

  if a.year then parts[#parts + 1] = tostring(a.year) end

  local discs = 1

  for _, s in ipairs(a.songs) do discs = math.max(discs, s.disc or 1) end

  if discs > 1 then parts[#parts + 1] = count(discs, "disc") end

  parts[#parts + 1] = count(a.count or #a.songs, "song")
  parts[#parts + 1] = length_words(a.duration_ms)

  if a.format then
    parts[#parts + 1] = a.lossless and a.rate
                        and ("%s %s kHz%s"):format(a.format, khz(a.rate),
                                                   a.bits and (" / %d-bit"):format(a.bits) or "")
                        or a.format
  end

  return table.concat(parts, " \u{00b7} ")
end

local function draw_album(g, w)
  local a = opened

  if not a then return 0 end

  draw_cover(g, a, 24, 20, BIG, BIG)

  local x = 24 + BIG + 26

  g:text(x, 20, "\u{2039} Albums", theme.text_dim, nil, "ui")
  g:text(x, 40, ui.fitted(a.title or "", w - x - 20, "title"), theme.text, nil, "title")
  g:text(x, 40 + gfx.height("title") + 2,
         ui.fitted(a.compilation and "Various artists" or a.artist or "", w - x - 20, "text"),
         theme.text_dim, nil, "text")
  g:text(x, 40 + gfx.height("title") + gfx.height("text") + 8,
         ui.fitted(album_facts(a), w - x - 20, "ui"), theme.text_dim, nil, "ui")

  -- Play and Shuffle are the kit's buttons, placed here (`place_buttons`).
  local y = 40 + gfx.height("title") + gfx.height("text") + 60
  local disc

  for i, s in ipairs(a.songs) do
    if (s.disc or 1) ~= disc and (a.songs[#a.songs].disc or 1) > 1 then
      disc = s.disc or 1
      g:text(x + 10, y + 6, ("DISC %d"):format(disc), theme.text_dim, nil, "ui")
      y = y + 26
    end

    local playing = is_playing(s)
    local on = view == "album" and i == selected

    if on then
      g:fill(x, y, w - x - 20, ROW, theme.accent)
    elseif playing then
      g:fill(x, y, w - x - 20, ROW, theme.mix(theme.window, theme.accent, 60))
    end

    local ink = on and theme.text_on or theme.text
    local dim = on and theme.text_on or theme.text_dim
    local ty = y + (ROW - gfx.height("ui")) // 2

    if playing then
      eq_bars(g, x + 14, y + 10)
    else
      g:text(x + 10, ty, tostring(s.track or i), dim, nil, "ui")
    end

    g:text(x + 44, ty, ui.fitted(s.title or "", w - x - 260, "ui"), ink, nil, "ui")

    local len = s.duration_ms and minutes(s.duration_ms) or ""

    draw_stars(g, w - 160, y + (ROW - 15) // 2, s.rating)
    g:text(w - 40 - gfx.measure(len, "ui"), ty, len, dim, nil, "ui")

    s.row_y = y
    y = y + ROW
  end

  return math.max(y, 20 + BIG) + 20
end

local function draw_face(g, x, y, size, name)
  g:fill_round(x, y, size, size, swatch(name), size // 2)

  local mark = initials(name)

  g:text(x + (size - gfx.measure(mark, "ui")) // 2, y + (size - gfx.height("ui")) // 2,
         mark, 0xffffffff, nil, "ui")
end

local ARTIST_LIST = 230

local function artist_facts(a)
  local songs, ms, hr = 0, 0, 0

  for _, al in ipairs(a.albums) do
    songs = songs + (al.count or 0)
    ms = ms + (al.duration_ms or 0)
    if hi_res(al) then hr = hr + 1 end
  end

  local parts = { count(#a.albums, "album"), count(songs, "song"), length_words(ms) }

  if hr > 0 then parts[#parts + 1] = ("%d in hi-res"):format(hr) end
  if #a.appears_on > 0 then parts[#parts + 1] = ("on %s"):format(count(#a.appears_on, "compilation")) end

  return table.concat(parts, " \u{00b7} ")
end

local function artist_songs(a)
  local out = {}

  for _, al in ipairs(a.albums) do
    for _, s in ipairs(al.songs) do out[#out + 1] = s end
  end

  table.sort(out, function(x, y)
    if (x.plays or 0) ~= (y.plays or 0) then return (x.plays or 0) > (y.plays or 0) end
    return (x.title or ""):lower() < (y.title or ""):lower()
  end)

  for i = #out, 6, -1 do out[i] = nil end

  return out
end

local function draw_artists(g, w)
  local th = gfx.height("ui")

  -- The list scrolls with the view; the pane stays.
  for i, a in ipairs(artists) do
    local y = 8 + (i - 1) * (FACE + 10)

    if a == artist_on then g:fill_round(8, y - 3, ARTIST_LIST - 16, FACE + 6, theme.line, 7) end

    draw_face(g, 16, y, FACE, a.name)
    g:text(16 + FACE + 10, y + (FACE - th) // 2, ui.fitted(a.name, ARTIST_LIST - FACE - 44, "ui"),
           a == artist_on and theme.text or theme.text_dim, nil, "ui")
  end

  local listed = 8 + #artists * (FACE + 10)
  local a = artist_on

  if not a then return listed end

  local px, py = ARTIST_LIST, top + 18
  local pw = w - ARTIST_LIST

  g:fill(px, top, pw, main.h, theme.window)
  g:fill(px, top, 1, main.h, theme.line_soft)

  local pane = g:push(px, 0, pw, top + main.h)
  g:text(22, py, ui.fitted(a.name, pw - 44, "title"), theme.text, nil, "title")
  g:text(22, py + gfx.height("title") + 2, ui.fitted(artist_facts(a), pw - 44, "ui"),
         theme.text_dim, nil, "ui")

  local list = {}

  for _, al in ipairs(a.albums) do list[#list + 1] = al end
  for _, al in ipairs(a.appears_on) do list[#list + 1] = al end

  local saved_top = top
  local cy = py + gfx.height("title") + gfx.height("ui") + 20
  local used = draw_cards(g, list, cy, pw, CARD, function(al) return al.year and tostring(al.year) or "" end)

  top = saved_top

  local sy = cy + used + 6

  g:text(22, sy, "MOST PLAYED", theme.text_dim, nil, "ui")
  draw_songs(g, artist_songs(a), sy + 22, pw, -1, true)
  g:pop(pane)
  return listed
end

local function draw_results(g, w)
  local r = found or { songs = {}, albums = {}, artists = {} }
  local y = 14

  local function heading(text)
    g:text(22, y, text, theme.text_dim, nil, "ui")
    y = y + 24
  end

  if #r.albums > 0 then
    heading("ALBUMS")
    y = y + draw_cards(g, r.albums, y, w, 120, album_sub)
  end

  if #r.songs > 0 then
    heading("SONGS")
    y = y + draw_songs(g, r.songs, y, w, view == "search" and selected or -1, true)
    y = y + 14
  end

  if #r.artists > 0 then
    heading("ARTISTS")

    local x = 22

    for _, a in ipairs(r.artists) do
      draw_face(g, x, y, FACE, a.name)
      g:text(x + FACE + 8, y + (FACE - gfx.height("ui")) // 2, a.name, theme.text_dim, nil, "ui")
      x = x + FACE + 26 + gfx.measure(a.name, "ui")
    end

    y = y + FACE + 14
  end

  if #r.albums + #r.songs + #r.artists == 0 then
    g:text(22, y, ("Nothing in the library matches \u{201c}%s\u{201d}."):format(query),
           theme.text_dim, nil, "ui")
  end

  return y
end

-- The albums' toolbar: Sort, as the drawing has it.
local album_sort_box = ui.dropdown{ name = "Sort albums", hidden = true,
                                    choices = { { "artist", "Artist" }, { "title", "Title" },
                                                { "year", "Year" }, { "added", "Date added" } },
                                    value = "artist" }
local TOOLBAR = 40

local content_h = 0

function main:draw(g)
  g:fill(0, 0, self.w, self.h, theme.window)

  local saved = g:push(0, -top, self.w, self.h + top)

  -- Scrolled: everything below is drawn in the content's coordinates,
  -- `top` above the view, and `push` keeps the clip to the view itself.
  if lib.pass == nil and next(lib.by_path) == nil and view ~= "search" then
    g:text(22, top + 22, "There is no music in " .. table.concat(roots, ", ") .. " yet.",
           theme.text_dim, nil, "text")
    g:text(22, top + 22 + gfx.height("text") + 4,
           "Songs put there appear here by themselves: FLAC, ALAC, AAC, MP3, WAV, AIFF, "
           .. "Vorbis and Opus.", theme.text_dim, nil, "ui")
    content_h = 0
  elseif view == "songs" then
    content_h = draw_songs(g, rows, 0, self.w, selected, false, source ~= "songs")
  elseif view == "albums" then
    content_h = TOOLBAR + draw_cards(g, albums, TOOLBAR + 6, self.w, CARD, album_sub)
    g:text(20, 12, "Sort", theme.text_dim, nil, "ui")
  elseif view == "album" then
    content_h = draw_album(g, self.w)
  elseif view == "artists" then
    content_h = draw_artists(g, self.w)
  elseif view == "search" then
    content_h = draw_results(g, self.w)
  end

  g:pop(saved)
end

-- Scrolled so the chosen row is in view.
local function keep_in_view()
  if view ~= "songs" then return end

  local y = HEAD_ROW + (selected - 1) * ROW

  if y < top + HEAD_ROW then top = math.max(0, y - HEAD_ROW) end
  if y + ROW > top + main.h then top = y + ROW - main.h end
end

local function scroll_by(dy)
  top = math.max(0, math.min(top + dy, math.max(0, content_h - main.h + 20)))
  place_buttons()
  win.dirty = true
end

function main:wheel(n)
  scroll_by(-n * ui.WHEEL_ROWS * ROW)
  return true
end

-- An album opened, from wherever it was chosen.
local function open_album(a)
  opened = a
  view = "album"
  selected = 1
  top = 0
  relayout()
  print(("music: opened %s, %s"):format(a.title or "?", count(a.count or #a.songs, "song")))
end

local function choose_view(name)
  view = name
  top = 0
  selected = 1

  if name == "songs" or name == "albums" or name == "artists" then
    views.on = ({ songs = 1, albums = 2, artists = 3 })[name]
  end

  if name == "artists" and not artist_on then artist_on = artists[1] end

  relayout()
end

-- The card at a point in a grid drawn from `y0`, or nil.
local function card_at(list, x, y, y0, w, size)
  size = size or CARD

  local n = per_row(w, size)
  local col = (x - 20) // (size + GAP)
  local row = (y - y0) // card_h(size)

  if col < 0 or col >= n or row < 0 then return nil end
  if (x - 20) % (size + GAP) > size or (y - y0) % card_h(size) > size + 2 * gfx.height("ui") + 8 then
    return nil
  end

  return list[row * n + col + 1]
end

local last_press = { at = 0, row = nil }

local function double(row)
  local t = sys.ticks()
  local again = last_press.row == row and (t - last_press.at) <= ui.double_click_ticks()

  last_press.at, last_press.row = t, row
  return again
end

-- Which list the main view's rows are, and the list a play gives the queue.
local function current_list()
  if view == "album" and opened then return opened.songs end
  if view == "search" and found then return found.songs end

  return rows
end

function main:mouse(action, x, y)
  if action ~= "press" then return action == "release" or action == "move" end

  win:focus_on(main)

  local cy = y + top

  if view == "songs" then
    if cy < HEAD_ROW then
      -- A column's name: sorted by it, and again the other way.
      local cols = column_widths(self.w)

      for i, c in ipairs(cols) do
        if x >= c.x - 10 and x < c.x + c.w and COLUMNS[i].name ~= "" then
          local key = COLUMNS[i].key

          if sort == key then descending = not descending else sort, descending = key, false end

          reshow()
          print(("music: sorted by %s%s"):format(key, descending and ", descending" or ""))
          return true
        end
      end

      return true
    end

    local i = (cy - HEAD_ROW) // ROW + 1

    if rows[i] then
      selected = i
      if double(i) then play_list(rows, i) end
      win.dirty = true
    end
  elseif view == "albums" then
    local a = card_at(albums, x, cy, TOOLBAR + 6, self.w)

    if a then open_album(a) end
  elseif view == "album" and opened then
    if cy < 20 + gfx.height("ui") + 4 and x > 24 + BIG + 20 and x < 24 + BIG + 120 then
      choose_view("albums")
      return true
    end

    for i, s in ipairs(opened.songs) do
      if s.row_y and cy >= s.row_y and cy < s.row_y + ROW and x > 24 + BIG + 26 then
        selected = i
        if double(i) then play_list(opened.songs, i) end
        win.dirty = true
      end
    end
  elseif view == "artists" then
    if x < ARTIST_LIST then
      local i = (cy - 8) // (FACE + 10) + 1

      if artists[i] then
        artist_on = artists[i]
        win.dirty = true
      end
    elseif artist_on then
      local list = {}

      for _, al in ipairs(artist_on.albums) do list[#list + 1] = al end
      for _, al in ipairs(artist_on.appears_on) do list[#list + 1] = al end

      local y0 = 18 + gfx.height("title") + gfx.height("ui") + 20
      local a = card_at(list, x - ARTIST_LIST, y, y0, self.w - ARTIST_LIST)

      if a then open_album(a) end
    end
  elseif view == "search" and found then
    -- The albums' row, then the songs' rows, as `draw_results` lays them.
    local y0 = 14

    if #found.albums > 0 then
      y0 = y0 + 24
      local a = card_at(found.albums, x, cy, y0, self.w, 120)

      if a then open_album(a) return true end

      y0 = y0 + ((#found.albums + per_row(self.w, 120) - 1) // per_row(self.w, 120)) * card_h(120)
    end

    if #found.songs > 0 then
      y0 = y0 + 24

      local i = (cy - y0) // ROW + 1

      if cy >= y0 and found.songs[i] then
        selected = i
        if double(i) then play_list(found.songs, i) end
        win.dirty = true
      end
    end
  end

  return true
end

function main:key(c)
  local k, mods = ui.keyparts(c)
  local list = current_list()

  if k == ui.UP and selected > 1 then
    selected = selected - 1
  elseif k == ui.DOWN and selected < #list then
    selected = selected + 1
  elseif (c == 13 or c == 10) and list[selected] then
    play_list(list, selected)
  elseif k == ui.PAGEDOWN then
    scroll_by(main.h - ROW)
    return true
  elseif k == ui.PAGEUP then
    scroll_by(-(main.h - ROW))
    return true
  else
    return false
  end

  keep_in_view()
  win.dirty = true
  return true
end

--------------------------------------------------------------------------
-- What the published controls say, for Automations and the gate: what is
-- playing and the badge, read as a person reads them.
--------------------------------------------------------------------------

local said_now = ui.view{ x = 0, y = 0, w = 0, h = 0, control = "display", name = "Now playing" }

function said_now:read()
  local s = now.song

  if not s then return "" end

  return ("%s \u{2014} %s \u{2014} %s"):format(s.title or "", s.artist or "", s.album or "")
end

local said_badge = ui.view{ x = 0, y = 0, w = 0, h = 0, control = "display", name = "Badge" }

function said_badge:read()
  return (badge_of(now.source)) or ""
end

--------------------------------------------------------------------------
-- Laying it out, as the view and the size say.
--------------------------------------------------------------------------

function place_buttons()
  local in_album = view == "album" and opened ~= nil

  play_btn.hidden, shuffle_btn.hidden = not in_album, not in_album
  album_sort_box.hidden = view ~= "albums"

  if in_album then
    local x = SIDE + 24 + BIG + 26
    local y = TOP - top + 40 + gfx.height("title") + gfx.height("text") + 22

    play_btn.x, play_btn.y = x, y
    shuffle_btn.x, shuffle_btn.y = x + play_btn.w + 10, y

    -- Scrolled off the top of the view, they are not there to press.
    if y < TOP then play_btn.hidden, shuffle_btn.hidden = true, true end
  end

  if view == "albums" then
    album_sort_box.x, album_sort_box.y = SIDE + 56, TOP + 6 - top
    if album_sort_box.y < TOP then album_sort_box.hidden = true end
  end
end

function relayout()
  local n_songs = 0

  for _ in pairs(lib.by_path) do n_songs = n_songs + 1 end

  if view == "songs" then
    local names = { songs = "Songs" }
    local title = names[source] or source:gsub("^%a+:", "")

    header.title = title
    header.sub = count(#rows, "song")
  elseif view == "albums" then
    header.title, header.sub = "Albums", count(#albums, "album")
  elseif view == "album" and opened then
    header.title, header.sub = opened.title or "Album", album_sub(opened)
  elseif view == "artists" then
    header.title, header.sub = "Artists", count(#artists, "artist")
  elseif view == "search" then
    local r = found or { songs = {}, albums = {} }

    header.title = "Search"
    header.sub = ("%s, %s"):format(count(#r.songs, "song"), count(#r.albums, "album"))
  end

  if side then
    side.items = sidebar_items()

    if view == "albums" or view == "album" then
      side.selected = "albums"
    elseif view == "artists" then
      side.selected = "artists"
    elseif view == "songs" then
      side.selected = source
    else
      side.selected = nil
    end
  end

  place_buttons()
  win.dirty = true
end

play_btn.on_click = function()
  if opened then
    if q and q.shuffled then q:shuffle(false) end
    play_list(opened.songs, 1)
  end
end

shuffle_btn.on_click = function()
  if opened and q then
    q.shuffled = true
    local first = 1 + (sys.ticks() % math.max(1, #opened.songs))

    play_list(opened.songs, first)
  end
end

album_sort_box.on_change = function(_, value)
  album_sort = value
  reshow()
end

views.on_change = function(_, i)
  query = ""
  search.text = ""
  choose_view(({ "songs", "albums", "artists" })[i])
end

function search:on_change()
  query = self.text or ""

  if query == "" then
    choose_view("songs")
    return
  end

  found = lib:search(query)
  view = "search"
  top, selected = 0, 1
  relayout()
  print(("music: search %q: %s, %s, %s"):format(query, count(#found.songs, "song"),
        count(#found.albums, "album"), count(#found.artists, "artist")))
end

function search:on_enter()
  if found and found.songs[1] then play_list(found.songs, 1) end
end

side = ui.sidebar{ x = 0, y = TOP + 8, w = SIDE - 1, h = H - TOP - FOOT - 12,
                   follow = { "left", "top", "bottom" },
                   items = sidebar_items(), selected = "songs",
                   on_select = function(_, id)
                     query, search.text = "", ""

                     if id == "albums" or id == "artists" then
                       choose_view(id)
                     else
                       source = id
                       rows = songs_of(source)
                       choose_view("songs")
                     end
                   end }

function win:on_resize(w, h)
  W, H = w, h
  place_now(w)
  relayout()
end

--
-- **The keys** (`player.md`, tier 1): Space plays and pauses, Return plays
-- the row chosen (the list's own), Control F searches, Control L shows the
-- song playing, Escape goes back from an album. What a focused field takes
-- - a space typed into the search - never reaches here.
--
function win:on_key(c)
  if c == 32 then
    if q and now.song then q:toggle() else play_list(current_list(), selected) end
    win.dirty = true
    return true
  elseif c == 6 then
    win:focus_on(search)
    return true
  elseif c == 12 then
    if now.song then
      source = "songs"
      rows = songs_of(source)
      choose_view("songs")

      for i, s in ipairs(rows) do
        if s.path == now.song.path then selected = i end
      end

      keep_in_view()
    end
    return true
  elseif c == 27 and view == "album" then
    choose_view("albums")
    return true
  end

  return false
end

--------------------------------------------------------------------------
-- The window's clock: the library refreshed, the queue fed, the server
-- asked what it did. Never in a paint.
--------------------------------------------------------------------------

local function start_refresh(folders)
  if refreshing then return end

  lib:start(folders)
  refreshing = true
  began_refresh = sys.ticks()
end

-- Once the library has the song asked for, its album plays from it.
local function play_given()
  local s = given and lib:song(given)

  if not s then return false end

  local a = s.album_id and lib:album(s.album_id)
  local list = a and a.songs or { s }
  local at = 1

  for i, x in ipairs(list) do
    if x.path == s.path then at = i end
  end

  given = nil

  if a then open_album(a) end

  play_list(list, at)
  return true
end

local asked_server = 0
local asked_device = 0
local painted = 0
local first_pass = true

function win:on_frame()
  local t = sys.ticks()
  local changed = false

  if first_pass then
    first_pass = false
    place_now(W)

    -- The song asked for: its own folder read first, so it plays at once,
    -- and then the whole library.
    start_refresh(given and { files.parent(given) } or nil)
  end

  if refreshing then
    local done, counts = lib:step(4)

    if done then
      refreshing = false
      print(("music: library %s, %s, %s (read %d, kept %d) in %d ms"):format(
        count(#(lib:songs()), "song"), count(#lib:albums(), "album"),
        count(#lib:artists(), "artist"), counts and counts.read or 0, counts and counts.kept or 0,
        (sys.ticks() - began_refresh) * 1000 // hz))
      reshow()

      if given then
        play_given()

        -- Then everything else; the song's folder was the first pass.
        start_refresh()
      end
    end

    changed = true
  end

  if q then
    q:tick()
    now = q:now()

    -- Four times a second: what the server did to this stream, and its peak.
    if now.song and t - asked_server > hz // 4 then
      asked_server = t

      local id = q:stream_id()

      for _, st in ipairs(audio.streams() or {}) do
        if st.stream == id then
          verdict = { exact = st.exact, why = st.why }
          peak = (st.peak or 0) / 32767
        end
      end

      local badge = badge_of(now.source)

      if badge ~= badge_said and verdict then
        badge_said = badge
        print("music: badge " .. tostring(badge))
      end
    end

    -- Every few seconds: which device plays.
    if t - asked_device > hz * 3 then
      asked_device = t

      local devices = audio.devices()

      for _, d in ipairs(devices or {}) do
        if d.playing then
          device_name = d.name .. (d.kind == "usb" and " \u{00b7} USB" or "")
        end
      end
    end

    -- Painted ten times a second while playing: the bar, the time, the meter.
    if q.playing and t - painted > hz // 10 then
      painted = t
      changed = true
    end

    self.poll_wait_ticks = (q.playing or refreshing) and 1 or nil
  else
    self.poll_wait_ticks = refreshing and 1 or nil
  end

  return changed
end

-- A folder's pulse: what changed in it read again, a slice at a time.
win:watch_folder(MUSIC, function() start_refresh() end)

for _, v in ipairs({ side_ground, side, header, nowv, main, play_btn, shuffle_btn,
                     album_sort_box, said_now, said_badge }) do
  win:add(v)
end

place_now(W)
relayout()
win:focus_on(main)

if audio.format().period == 0 then print("music: this machine has no sound device") end

win:run()

if q then q:close() end
