-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Music: what is playing on top, the library underneath.
-- kosmos: application
-- kosmos: icon App_MediaPlayer
--
--   wm music                      everything in /home
--   wm music:/home/Music          everything in there
--   wm music:/home/groove.mp3     that folder, with that track picked
--
-- **The window drawn in `docs/music.html`**, which Diego approved on 14
-- September and which is the pilot of a second look for the whole system: flat
-- rather than dimensional, dark with an orange accent, a title larger than the
-- desktop's text. `ui.md` 16.8b still says Kosmos is dimensional on purpose,
-- and that stands until he has used this and decided whether the rest follow.
--
-- **Six things had to exist before this window could be drawn**, and each is
-- a `testing.md` section of its own: a picture drawn at another size (18.78),
-- a heading larger than the desktop's text (18.79), a cover lifted out of an
-- MP3 without becoming a file (18.80), a window that can ask for its own size
-- (18.81), a drawing command that carries that size (18.82) and a triangle
-- command (18.83). The last two were found by asking how this window would
-- actually draw a sleeve and a play arrow, rather than by assuming it could.
--
-- **What is borrowed rather than rebuilt**: the playing itself is
-- `/lib/media.lua`'s - reading, decoding, feeding, the clock and seeking - and
-- so is the pacing that cost half the playback speed when it was confused,
-- the peak meter read from the server rather than from a progress bar, and
-- saying *why* a folder could not be listed instead of showing an empty list.

local ui    = use("/lib/ui.lua")
local audio = use("/lib/audio.lua")
local media = use("/lib/media.lua")

--------------------------------------------------------------------------
-- The look.
--
-- **Its own palette, not the desktop's**, which is what makes this a pilot
-- rather than a theme: VOX's dark and orange as `docs/music.html` draws it,
-- and a light one beside it. The desktop's theme decides every other window;
-- this one is being tried.
--------------------------------------------------------------------------

local LOOKS = {
  dark = {
    ground = 0xff242426, panel = 0xff2e2e31, raised = 0xff38383c,
    line   = 0xff3f3f44, ink   = 0xffececec, dim    = 0xff9a9aa0,
    faint  = 0xff6d6d73, accent = 0xfff28a2e, knob = 0xffffffff,
  },
  light = {
    ground = 0xfff6f6f4, panel = 0xffecebe8, raised = 0xffdcdbd7,
    line   = 0xffd6d5d1, ink   = 0xff1e1e1e, dim    = 0xff6c6c68,
    faint  = 0xff9a9a95, accent = 0xffe07a1f, knob = 0xff1e1e1e,
  },
}

local look = "dark"
local P = LOOKS[look]

--------------------------------------------------------------------------
-- Where things are. One table rather than numbers in the drawing, because
-- the design gives them and a layout argued with in one place is a layout
-- that can be argued with.
--------------------------------------------------------------------------

local W, H      = 380, 520
local MINI_W, MINI_H = 330, 116
local PAD       = 14
local COVER     = 78
local ROW_H     = 58
local ROW_ART   = 44
local TITLE_PX  = 22

local NOW_H     = 102
local TIMES_Y   = NOW_H
local SEEK_Y    = TIMES_Y + 24
local TRANS_Y   = SEEK_Y + 12
local TRANS_H   = 46
local VOL_Y     = TRANS_Y + TRANS_H
local SRC_Y     = VOL_Y + 26
local SRC_H     = 42
local SUB_Y     = SRC_Y + SRC_H
local LIST_Y    = SUB_Y + 28
local FOOT_H    = 24

--------------------------------------------------------------------------
-- Which folder, and what to start on. Unchanged: a file rather than a folder
-- is what Tracker sends when somebody opens an MP3.
--------------------------------------------------------------------------

local FOLDER = "/home"
local START

do
  local given = tostring(args or ""):match("^%s*(%S+)")

  if given and given:sub(1, 1) == "/" then
    local attrs = fs.getattr(given)

    if attrs and attrs.kind == "directory" then
      FOLDER = given
    else
      FOLDER = given:match("^(.*)/") or "/home"
      START = given:match("([^/]+)$")
    end
  end
end

local win, err = ui.window{ title = "Music", w = W, h = H, x = 180, y = 90 }

if not win then
  print("music: " .. tostring(err))
  return
end

--
-- **The size to lay out against is the one the window came back with.** A
-- window asks and the compositor answers with what it gave, which is not the
-- same number: the frame takes a border either side. Laying out against the
-- asked-for width put the right-hand end of every right-aligned string past
-- the edge, and the picture of this window showed it as `on /hom`.
--
-- **And it is `win.root`, not `win.width`.** The published `width` is a
-- property served to whoever asks *about* this window - `w:publish("width",
-- ...)` - and reading it as a field gives nil, which left the layout at the
-- number that was wrong. The root view is built from the reply, so its size
-- is the client area itself.
--
W = win.root.w
H = win.root.h

--------------------------------------------------------------------------
-- The library.
--
-- Names first, tags when a track is chosen. `tags.read` costs one small read
-- per file and a cover costs none - it is found in place rather than read -
-- so a thousand songs list without decoding a thousand pictures.
--------------------------------------------------------------------------

local tracks = {}
local listed, list_why = fs.list(FOLDER)

for _, f in ipairs(listed or {}) do
  local low = tostring(f):lower()

  if low:match("%.wav$") or low:match("%.mp3$") then
    tracks[#tracks + 1] = { file = f, title = f:gsub("%.%w+$", "") }
  end
end

local trouble

if not listed then
  trouble = ("could not list %s: %s"):format(FOLDER, tostring(list_why))
  print("music: " .. trouble)
elseif #tracks == 0 then
  trouble = "nothing to play in " .. FOLDER
  print(("music: nothing to play among %d names in %s"):format(#listed, FOLDER))
end

local chosen = 1

if START then
  for i, t in ipairs(tracks) do
    if t.file == START then chosen = i break end
  end
end

--------------------------------------------------------------------------
-- What is playing.
--------------------------------------------------------------------------

local player, cover_name
local status = trouble or "nothing loaded"

local function unload()
  if player then player:close() end

  player, cover_name = nil, nil
end

--
-- A track's tags, read once and kept on the track itself. The cover is asked
-- for separately and only for what is playing, because that is the one
-- picture on the screen.
--
local function tags_for(t)
  if t.tags then return t.tags end

  t.tags = media.tags(FOLDER .. "/" .. t.file) or {}

  return t.tags
end

--
-- **The chosen track's cover, whether or not it is playing.** A library is
-- looked at as much as it is listened to, and the picture is the first thing
-- a person recognises. Fetched once per track: the name is what the
-- compositor caches, and asking again for one it already holds costs a
-- message rather than a decode.
--
local function cover_for(i)
  local t = tracks[i]

  if not t then cover_name = nil return end

  tags_for(t)

  if t.cover_name == nil then
    t.cover_name = media.cover(FOLDER .. "/" .. t.file) or false
  end

  cover_name = t.cover_name or nil
end

--
-- **How often this window wakes**, declared here and set below.
--
-- Two numbers that are not the same number, and getting them wrong costs
-- half the playback speed - `media.lua` and the window that came before this
-- one both learned it. Forward-declared because the transport presses play
-- and the pacing is written where the tick loop is.
--
local pace

local function load(i)
  local t = tracks[i]

  if not t then return end

  unload()

  local p, why = media.open(FOLDER .. "/" .. t.file)

  if not p then status = tostring(why) return end

  player = p
  chosen = i
  cover_for(i)
  status = nil
  player:play()

  -- **Awake often enough to feed the server.** Without this the window went
  -- on waking once a second, handed over twelve periods a turn, and the
  -- sound was a third of a second and then nothing - which is what
  -- `run_media.py` measured when the play arrow was first wired up.
  pace()
end

--------------------------------------------------------------------------
-- The drawn controls.
--
-- The vendored Haiku icons are applications, files, folders and devices -
-- there is no play, pause or skip among them - so these are drawn, which
-- Diego chose over vendoring more or generating pictures. Rectangles and
-- triangles, sharp at any size, and the triangle is a command as of
-- `testing.md` 18.83.
--------------------------------------------------------------------------

local function arrow(g, x, y, s, colour, back)
  if back then
    g:triangle(x + s, y, x + s, y + s, x, y + s / 2, colour)
  else
    g:triangle(x, y, x, y + s, x + s, y + s / 2, colour)
  end
end

local function draw_play(g, x, y, s, colour)
  arrow(g, x + s * 0.12, y, s * 0.82, colour)
end

local function draw_pause(g, x, y, s, colour)
  local bar = math.max(2, math.floor(s / 3.2))

  g:fill(x + bar // 2, y, bar, s, colour)
  g:fill(x + s - bar - bar // 2, y, bar, s, colour)
end

local function draw_skip(g, x, y, s, colour, back)
  local bar = math.max(2, math.floor(s / 6))

  if back then
    g:fill(x, y, bar, s, colour)
    arrow(g, x + bar + 1, y, s - bar - 1, colour, true)
  else
    arrow(g, x, y, s - bar - 1, colour)
    g:fill(x + s - bar, y, bar, s, colour)
  end
end

--
-- Two lines that cross, with a head on each: at eighteen pixels the shape has
-- to be read at a glance, and the four-rectangle version of it was a scribble
-- on the screen (the first picture of this window, 15 September).
--
local function draw_shuffle(g, x, y, s, colour)
  local t = math.max(2, math.floor(s / 8))

  g:fill(x, y + t, s - t * 3, t, colour)
  g:fill(x, y + s - t * 2, s - t * 3, t, colour)
  arrow(g, x + s - t * 3, y, t * 3, colour)
  arrow(g, x + s - t * 3, y + s - t * 3, t * 3, colour)
end

local function draw_repeat_(g, x, y, s, colour)
  local t = math.max(2, math.floor(s / 7))

  g:fill(x, y, s - t * 2, t, colour)
  g:fill(x, y, t, s // 2, colour)
  g:fill(x + t * 2, y + s - t, s - t * 2, t, colour)
  g:fill(x + s - t, y + s // 2, t, s // 2, colour)
  arrow(g, x + s - t * 3, y - t, t * 3, colour)
end

local function draw_queue(g, x, y, s, colour)
  local t = math.max(2, math.floor(s / 8))

  for i = 0, 2 do
    g:fill(x, y + i * (t * 3), s - (i == 2 and s // 3 or 0), t, colour)
  end
end

local function draw_mini(g, x, y, s, colour)
  g:frame(x, y, s, s * 0.8, colour)
  g:fill(x + 2, y + s * 0.5, s - 4, s * 0.3 - 2, colour)
end

--------------------------------------------------------------------------
-- What is playing, across the top.
--------------------------------------------------------------------------

--
-- **Where a right-aligned string starts.**
--
-- `gc:text` clips by whole character cells, and a cell is the width of "0" in
-- the face it draws with. On a proportional face that is wider than the
-- average letter, so a string placed with exactly its measured width of room
-- loses its last characters to the clip - which is why the footer read
-- `on /hom` through three attempts at fixing it, while the shorter strings
-- above it looked fine. Measured, not guessed: the client area really is 380
-- wide and `on /home` really is 64 pixels.
--
-- So the room left is rounded up to whole cells, and one more for the edge.
--
local function right_of(width, s)
  local cell = math.max(1, gfx.measure("0", "ui"))
  local wide = gfx.measure(s, "ui")

  return width - PAD - (math.ceil(wide / cell) + 1) * cell
end

local function clock(secs)
  local whole = math.floor(secs or 0)

  return ("%d:%02d"):format(whole // 60, whole % 60)
end

local now = ui.view{ x = 0, y = 0, w = W, h = SEEK_Y + 12 }

function now:draw(g)
  local t = tracks[chosen]
  local tags = t and t.tags or {}
  local info = player and player.info
  local left = PAD + COVER + 12

  g:fill(0, 0, self.w, self.h, P.ground)

  --
  -- The cover, at the size the design draws it: five hundred pixels of sleeve
  -- into 78, through the scaler. No cover, and the file's own icon sits in a
  -- panel instead - which is what the vendored Haiku set has for this.
  --
  if cover_name then
    g:picture(PAD, 12, COVER, COVER, cover_name)
  else
    g:fill(PAD, 12, COVER, COVER, P.raised)
    g:icon(PAD + (COVER - 32) // 2, 12 + (COVER - 32) // 2, "File_Audio", 32)
  end

  --
  -- The chips: what the file is, in its own words. `197 kbps VBR` is the
  -- average a Xing header gives, since `testing.md` 18.77 - it said `64` and
  -- `9:58` for a three-minute song before that.
  --
  local chips, x = {}, left

  if info then
    chips[#chips + 1] = info.format
    if info.bitrate then
      chips[#chips + 1] = ("%d kbps%s"):format(info.bitrate,
                                              info.vbr and " VBR" or "")
    end
    chips[#chips + 1] = ("%.1f kHz"):format(info.rate / 1000)
  end

  if tags.genre then chips[#chips + 1] = "#" .. tags.genre:lower() end

  for i, c in ipairs(chips) do
    local w = gfx.measure(c, "ui") + 10

    if x + w > self.w - PAD then break end

    g:frame(x, 14, w, 18, i == 1 and P.accent or P.line)
    g:text(x + 5, 18, c, i == 1 and P.accent or P.dim)
    x = x + w + 6
  end

  local who = tags.artist or "unknown artist"

  if tags.album then who = who .. "  -  " .. tags.album end

  g:text(left, 40, who, P.dim)
  g:text(left, 56, t and (tags.title or t.title) or "nothing to play",
         P.ink, nil, "ui", TITLE_PX)

  --
  -- Where we are, from what came out of the speaker rather than from what was
  -- handed over - `media.lua` says why those differ - and how much is left.
  --
  local at = player and player:position() or 0
  local total = (info and info.seconds) or 0

  g:text(PAD, TIMES_Y + 4, clock(at), P.dim)

  local remaining = "-" .. clock(math.max(0, total - at))

  g:text(right_of(self.w, remaining), TIMES_Y + 4, remaining, P.dim)

  -- The meter, off the audio server's own peak: a progress bar moves whether
  -- or not anything is coming out of the speaker.
  local peak = player and player:peak() or 0
  local bars = 5
  local mx = (self.w - bars * 6) // 2
  local top = TIMES_Y + 18

  for i = 1, bars do
    local share = 0.55 + 0.45 * (((i * 2) % 5) / 4)
    local tall = math.max(2, math.floor(13 * math.min(1, peak) * share))

    -- The well each bar stands in, so a silent meter is five dim marks
    -- rather than nothing at all - which is what says there is a meter.
    g:fill(mx + (i - 1) * 6, top - 13, 4, 13, P.raised)
    g:fill(mx + (i - 1) * 6, top - tall, 4, tall,
           player and P.accent or P.faint)
  end

  -- The bar, and the knob on it.
  local frac = (total > 0) and math.min(1, at / total) or 0
  local bw = self.w - PAD * 2

  g:fill(PAD, SEEK_Y, bw, 4, P.raised)
  g:fill(PAD, SEEK_Y, math.floor(bw * frac), 4, P.accent)
  g:fill(PAD + math.floor(bw * frac) - 4, SEEK_Y - 3, 9, 10, P.knob)
end

--
-- A click on the bar is a place to go; the knob is drawn on it and is not a
-- separate thing to hit.
--
function now:on_click(x, y)
  if not player or y < SEEK_Y - 6 or y > SEEK_Y + 12 then return end

  local frac = math.max(0, math.min(1, (x - PAD) / (self.w - PAD * 2)))

  player:seek(frac * (player.info.seconds or 0))
  win:paint()
end

--------------------------------------------------------------------------
-- The transport, and the volume under it.
--------------------------------------------------------------------------

local folded = false
local transport = ui.view{ x = 0, y = TRANS_Y, w = W, h = TRANS_H + 26 }

local function slot(i)
  return math.floor((W / 7) * (i - 0.5))
end

function transport:draw(g)
  local s = 18
  local playing = player and not player:finished()

  g:fill(0, 0, self.w, self.h, P.ground)

  draw_shuffle(g, slot(1) - s // 2, 14, s, P.faint)
  draw_skip(g, slot(2) - s // 2, 14, s, P.ink, true)

  if playing then
    draw_pause(g, slot(4) - 15, 8, 30, P.ink)
  else
    draw_play(g, slot(4) - 15, 8, 30, P.ink)
  end

  draw_skip(g, slot(5) - s // 2, 14, s, P.ink)
  draw_repeat_(g, slot(6) - s // 2, 16, s, P.accent)
  draw_queue(g, slot(7) - s // 2, 15, s, P.faint)

  g:fill(0, TRANS_H - 1, self.w, 1, P.line)

  -- The volume, with the one vendored icon that fits it.
  g:icon(PAD, TRANS_H + 6, "Misc_Speaker", 14)
  g:fill(PAD + 22, TRANS_H + 12, self.w - PAD * 2 - 22, 3, P.raised)
  g:fill(PAD + 22, TRANS_H + 12,
         math.floor((self.w - PAD * 2 - 22) * 0.72), 3, P.dim)
end

function transport:on_click(x, y)
  if y > TRANS_H then return end

  local which = math.min(7, math.max(1, math.floor(x / (W / 7)) + 1))

  if which == 4 then
    if player then
      if player.playing then player:pause() else player:play() end

      pace()
    else
      load(chosen)
    end
  elseif which == 2 then
    load(math.max(1, chosen - 1))
  elseif which == 5 then
    load(math.min(#tracks, chosen + 1))
  end

  win:paint()
end

--------------------------------------------------------------------------
-- Where the music comes from, and the list.
--------------------------------------------------------------------------

local SOURCES = { "Library", "Folders", "Playlists", "Queue", "Radio" }

local sources = ui.view{ x = 0, y = SRC_Y, w = W, h = SRC_H + 28 }

function sources:draw(g)
  g:fill(0, 0, self.w, self.h, P.ground)

  for i, name in ipairs(SOURCES) do
    local x = math.floor(self.w / 5 * (i - 1))
    local w = math.floor(self.w / 5)
    local on = (i == 1)
    local colour = on and P.ink or (i == 5 and P.faint or P.dim)

    g:text(x + (w - gfx.measure(name, "ui")) // 2, 16, name, colour)

    if on then g:fill(x + 8, SRC_H - 4, w - 16, 2, P.accent) end
  end

  g:fill(0, SRC_H, self.w, 1, P.line)
  g:text(PAD, SRC_H + 8, "Songs", P.accent)
  g:text(PAD + 52, SRC_H + 8, "Albums", P.dim)
  g:text(PAD + 112, SRC_H + 8, "Artists", P.dim)
end

local list = ui.view{ x = 0, y = LIST_Y, w = W, h = H - LIST_Y - FOOT_H }

function list:draw(g)
  g:fill(0, 0, self.w, self.h, P.ground)

  if trouble then
    g:text(PAD, 12, trouble, P.dim)
    return
  end

  local rows = math.floor(self.h / ROW_H)

  for i = 1, math.min(rows, #tracks) do
    local t = tracks[i]
    local y = (i - 1) * ROW_H
    local tags = t.tags or {}

    if i == chosen then
      g:fill(0, y, self.w, ROW_H, P.panel)
      g:fill(0, y, 3, ROW_H, P.accent)
    end

    if t.cover_name == nil and i <= 4 then
      -- Only the first few, and only once: a cover is a decode, and a
      -- library should not pay for pictures nobody has scrolled to.
      tags_for(t)
      t.cover_name = media.cover(FOLDER .. "/" .. t.file) or false
    end

    if t.cover_name then
      g:picture(PAD, y + 7, ROW_ART, ROW_ART, t.cover_name)
    else
      g:fill(PAD, y + 7, ROW_ART, ROW_ART, P.raised)
      g:icon(PAD + 6, y + 13, "File_Audio", 32)
    end
    g:text(PAD + ROW_ART + 10, y + 12, tags.title or t.title, P.ink)
    g:text(PAD + ROW_ART + 10, y + 30,
           (tags.artist or "unknown artist"), P.dim)

    local kind = t.file:lower():match("%.(%w+)$"):upper()

    g:text(right_of(self.w, kind), y + 30, kind, P.dim)
    g:fill(0, y + ROW_H - 1, self.w, 1, P.line)
  end
end

function list:on_click(x, y)
  local i = math.floor(y / ROW_H) + 1

  if tracks[i] then
    chosen = i
    cover_for(i)
    win:paint()
  end
end

local foot = ui.view{ x = 0, y = H - FOOT_H, w = W, h = FOOT_H }

function foot:draw(g)
  g:fill(0, 0, self.w, self.h, P.ground)
  g:fill(0, 0, self.w, 1, P.line)
  g:text(PAD, 6, ("%d songs"):format(#tracks), P.faint)

  local where = "on " .. FOLDER

  g:text(right_of(self.w, where), 6, where, P.faint)
end

--------------------------------------------------------------------------
-- Feeding, and painting at a rate a person can see. Both numbers are
-- `media.lua`'s lesson rather than this window's invention.
--------------------------------------------------------------------------

local counter_hz = (fs.read("/dev/cpu") or {}).counter_hz or 62500000

function pace()
  if player then
    win.poll_wait_ticks = 1
    win.tick_every = counter_hz // 250
  else
    win.poll_wait_ticks = nil
    win.tick_every = counter_hz
  end
end

local PAINT_EVERY = 10
local since_paint = 0
local ticker = ui.view{ x = 0, y = 0, w = 0, h = 0 }

function ticker:tick()
  if not player then return end

  player:tick()

  if player.error then
    status = player.error
    unload()
    pace()
    win:paint()
    return
  end

  if player:finished() then
    unload()
    pace()
    win:paint()
    return
  end

  since_paint = since_paint + 1

  if since_paint >= PAINT_EVERY then
    since_paint = 0
    win:paint()
  end
end

win:add(now)
win:add(transport)
win:add(sources)
win:add(list)
win:add(foot)
win:add(ticker)

if tracks[chosen] then cover_for(chosen) end

if audio.format().period == 0 then status = "this machine has no sound device" end

win:run()
