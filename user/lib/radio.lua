-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- **A radio station, heard** (`docs/flight.md` F9, `docs/music.md`'s Radio
-- source): an MP3 that never ends, from an HTTP or HTTPS address, played.
-- Flight's air traffic control is its first user, and Music's Radio wants
-- the same door.
--
--   local radio = use("/Kosmos/Libraries/radio.lua")
--   local r, why = radio.open("http://example.org/stream.mp3", { name = "Flight" })
--   r:tick()            -- each frame, or each pass of a loop: never waits
--   r:state()           -- { step, status, got, heard, dry, why }
--   r:close()
--
-- **Nothing here is new machinery**; it is three kits put together, which
-- is what a library is for (`CLAUDE.md`, *kits supply*):
--
-- - the Network Kit's download (`download.c`) - the connection, TLS, and
--   `httpc` streaming the body into a region, so a byte of it is never a
--   Lua string;
-- - the Playback Kit, an MP3 player on a thread of its own in the audio
--   band, which takes its frames from an address in that region;
-- - an audio stream to the audio server, the ring the player writes into.
--
-- Lua only says where: how much of the region the download has filled, and
-- how far the player has taken. The region is emptied when the player has
-- had all of it; until then the download stops reading, the connection's
-- window closes, and the station waits on its side - nothing is lost.
--
-- **A station that stops is asked again**: a connection that fails or ends
-- is begun again after `AGAIN_S` seconds, so a long flight keeps its radio.
-- A redirect is followed, up to `REDIRECTS` of them, to another host or
-- scheme - a station's address is very often a redirect to its server.
--
-- What waits: looking up a name, once a minute at most for a host (the
-- Network Kit caches it), and up to five seconds then. Everything else is a
-- step that does what it can and returns.

local http = use("/Kosmos/Libraries/http.lua")
local netstream = use("/Kosmos/Libraries/netstream.lua")
local regions = use("/Kosmos/Libraries/regions.lua")
local audio = use("/Kosmos/Libraries/audio.lua")
local playback = use("/Kosmos/Kits/playback")

local radio = {}

radio.REGION = 64 * 1024        -- four seconds of a 128 kbit/s station
radio.PIECE = 4096              -- what goes to the player at once
radio.REDIRECTS = 5
radio.AGAIN_S = 5               -- seconds before a station that stopped is asked again
radio.PERIODS = 8               -- the audio ring's depth: about 90 ms

local station = {}
station.__index = station

-- An address a redirect gave, made whole against the one it came from.
local function whole(location, parts)
  if location:match("^%a[%w+.%-]*://") then return location end
  if location:sub(1, 2) == "//" then return parts.scheme .. ":" .. location end
  if location:sub(1, 1) == "/" then return parts.scheme .. "://" .. parts.hostport .. location end
  return parts.scheme .. "://" .. parts.hostport .. parts.path:gsub("[^/]*$", "") .. location
end

local function counter_hz()
  return (fs.read("/Devices/cpu") or {}).counter_hz or 62500000
end

-- The download begun again at `address`: true, or nil and why.
function station:connect(address)
  if self.d then self.d:close() self.d = nil end
  self.fed, self.began = 0, false
  local parts, bad = http.split(address)
  if not parts then return nil, bad end
  local where, why, said = netstream.address(parts.host)
  if not where then
    return nil, ("cannot look up %s: %s"):format(parts.host, said or tostring(why))
  end
  local d, refused = fs.download("/Network", {
    address = where, port = parts.port, host = parts.host, path = parts.path,
    tls = parts.scheme == "https", from = 0, at = self.region.at, bytes = self.region.size,
    anchors = parts.scheme == "https" and netstream.trusted() or nil,
  })
  if not d then return nil, ("%s: %s"):format(parts.hostport, tostring(refused)) end
  self.d, self.parts, self.address = d, parts, address
  self.connects = self.connects + 1
  return true
end

--
-- A station at `address`, its sound to an audio stream named `opts.name`.
-- Nil and why when the machine has no sound, or the address is not one.
--
function radio.open(address, opts)
  opts = opts or {}
  local region, rwhy = regions.make(radio.REGION)
  if not region then return nil, rwhy end
  local player, pwhy = playback.open{ kind = "mp3" }
  if not player then
    regions.free(region)
    return nil, tostring(pwhy)
  end
  local out, owhy = audio.open(opts.name or "Radio", radio.PERIODS)
  if not out then
    player:close()
    regions.free(region)
    return nil, tostring(owhy)
  end
  local ok, swhy = pcall(player.seek, player, out.ring)
  if not ok then
    player:close()
    out:close()
    regions.free(region)
    return nil, tostring(swhy)
  end
  player:play()
  local self = setmetatable({
    region = region, player = player, out = out, first = address, address = address,
    fed = 0, redirects = 0, connects = 0, got = 0, status = 0, step = "begun",
    hz = counter_hz(), heard = {}, why = nil,
  }, station)
  local began, why = self:connect(address)
  if not began then self:stopped(why) end
  return self
end

-- The station gone quiet, and asked again in a while.
function station:stopped(why)
  if self.d then self.d:close() self.d = nil end
  self.step, self.why = "waiting", why
  self.again_at = sys.ticks() + radio.AGAIN_S * self.hz
end

--
-- What can move, moved: the download stepped, its bytes given to the
-- player as far as it has room, the region emptied once all of it went.
-- Returns the step: "sending", "head", "body", "waiting" or "closed".
--
function station:tick()
  if self.closed then return "closed" end
  if not self.d then
    if self.again_at and sys.ticks() >= self.again_at then
      self.again_at = nil
      self.redirects = 0
      local began, why = self:connect(self.first)
      if not began then self:stopped(why) end
    end
    return self.step
  end
  local d = self.d
  local step, failed = d:step()
  if step == "failed" then
    self:stopped(failed)
    return self.step
  end
  -- The head: a redirect followed, a refusal said, a station heard.
  if not self.began and (step == "body" or step == "done") then
    self.began = true
    self.status = d:status()
    if self.status >= 300 and self.status < 400 then
      local location = d:header("location")
      self.redirects = self.redirects + 1
      if not location or self.redirects > radio.REDIRECTS then
        self:stopped(location and "too many redirects" or ("a %d with nowhere to go"):format(self.status))
        return self.step
      end
      local began, why = self:connect(whole(location, self.parts))
      if not began then self:stopped(why) end
      return "head"
    end
    if self.status ~= 200 then
      self:stopped(("%s answered %d"):format(self.parts.hostport, self.status))
      return self.step
    end
    self.why = nil
  end
  -- The bytes the download put in the region, to the player.
  local ready = d:ready()
  while self.fed < ready do
    local n = math.min(radio.PIECE, ready - self.fed)
    if not self.player:frame(self.region.at + self.fed, n) then break end
    self.fed = self.fed + n
  end
  if ready > 0 and self.fed == ready then
    d:taken()
    self.got = self.got + ready
    self.fed = 0
  end
  -- A station's stream does not end; one that did is asked again.
  if step == "done" then
    self:stopped("the station ended its stream")
    return self.step
  end
  self.step = step
  return step
end

-- What is heard: the download's step and the player's account.
function station:state()
  local h = self.player:state(self.heard)
  return {
    step = self.step, status = self.status, got = self.got + (self.d and self.d:ready() or 0),
    address = self.address, connects = self.connects, why = self.why or (h.why ~= "" and h.why or nil),
    frames = h.frames_out, dry = h.dry, refused = h.refused, audio_band = h.audio_band,
  }
end

function station:close()
  if self.closed then return end
  self.closed = true
  if self.d then self.d:close() self.d = nil end
  self.player:close()
  self.out:close()
  regions.free(self.region)
end

return radio
