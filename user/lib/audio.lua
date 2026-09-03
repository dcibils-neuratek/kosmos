-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Playing a sound, from a program that does not own the sound device.
--
-- Almost nothing owns it: `SPAWN_AUDIO` goes to one process, the audio
-- server, and that is what makes per-application volume a thing that can
-- exist. If every program wrote to the device the last one would win, and
-- there would be nothing for a volume control to be a volume *of*.
--
-- So a program opens a *stream*, says what it is called - the name is what
-- appears in the Mixer - and sends periods. What comes back from `play` is
-- whether the server took it: false means the stream is already as far
-- ahead as it is allowed to be, and the caller should wait rather than
-- buffer, because the queue is the latency.

local audio = {}

--
-- The format, asked rather than assumed.
--
-- `sys.info()` reports what the *board* does, and this passes it on so a
-- caller can size its buffers without a constant of its own. Zero when the
-- machine has no sound device, which is a supported way to run.
--
function audio.format()
  local i = sys.info() or {}

  return {
    rate = i.audio_rate or 0,
    channels = i.audio_channels or 0,
    period = i.audio_period or 0,
    periods = i.audio_periods or 0,
    frames = (i.audio_period or 0) // (2 * math.max(1, i.audio_channels or 1)),
  }
end

local stream = {}
stream.__index = stream

--
-- Returns whether the server took it, and *why not* when it did not.
--
-- The second value matters more than it looks. `play` returning false has
-- two meanings - the stream is full, which is normal and means wait, and
-- the message did not arrive, which is a bug - and a caller that cannot
-- tell them apart yields for ever on the second one. That is exactly what
-- happened: two tones sat in the Mixer at "idle" while `beep` spun,
-- because a failed send and a full queue looked identical.
--
--
-- Hand over one period.
--
-- **Nothing is sent.** The samples go into the ring this stream opened
-- with, which the audio server is already reading from - `CLAUDE.md`'s
-- rule, and `audioring.h` has the reasoning. What used to happen here was
-- an IPC call with 1024 bytes of payload, 172 times a second, minting a
-- Lua string on each side of every one.
--
-- Returns false with "full" when the ring has no room, which means this
-- client is ahead of the device and should do something else for a moment.
-- That is a fact rather than a failure, and it is the only thing keeping
-- the pipe short.
--
function stream:play(pcm)
  if sys.ring_put(self.ring, pcm) then return true end

  return false, "full"
end

--
-- The same, but wait for room.
--
-- For a program whose whole job is playing - `play` at a prompt - as
-- against one with a window to keep answering, which wants `play` and its
-- "full" so it can go and do that instead. Two shapes of caller, not two
-- ways of doing one thing.
--
function stream:write(pcm)
  while true do
    if sys.ring_put(self.ring, pcm) then return true end

    --
    -- One tick. The ring holds `AUDIO_RING_PERIODS` and the device another
    -- few, so there is tens of milliseconds still to play while this
    -- sleeps, and being woken with room to spare is the whole idea.
    --
    sys.sleep(1)
  end
end

-- How many periods are waiting to be mixed, and how much room is left.
function stream:queued()
  return sys.ring_ready(self.ring) or 0
end

function stream:space()
  return sys.ring_space(self.ring) or 0
end

function stream:close()
  fs.send("/dev/audio", { type = "close", stream = self.id })
  if self.cap then sys.release(self.cap) end
end

--
-- A stream, named for the Mixer.
--
-- The name is the program's own and nothing checks it: two copies of the
-- same program get two streams with the same name, which is right - they
-- are two things making a noise and each should have its own fader.
--
-- **The client creates the ring and hands over a capability to it**, the
-- same way a window hands the compositor its surface. The server can reach
-- that ring and nothing else in this process, which is what makes shared
-- memory safe to use here at all: it is not "shared memory", it is one
-- region, named by a capability, given away deliberately.
--
function audio.open(name)
  local fmt = audio.format()

  if fmt.period == 0 then return nil, "this machine has no sound device" end

  local cap, at = sys.ring_create(fmt.period)

  if not cap then return nil, tostring(at) end

  -- The capability goes as `send`'s third argument, which is how a window
  -- hands the compositor its surface. It is not a field in the message: a
  -- capability is not data and does not travel as any.
  local r, why = fs.send("/dev/audio",
                         { type = "open", name = name }, cap)

  if not r or not r.ok then
    sys.release(cap)

    return nil, tostring((r and r.error) or why)
  end

  return setmetatable({ id = r.stream, period = r.period,
                        periods = r.periods, ring = at, cap = cap }, stream)
end

--
-- What is playing, for the Mixer and for anything else that wants to know.
--
-- `peak` is measured before the gain, so a muted stream still shows a
-- moving meter. That is deliberate: the meter answers "who is sending
-- audio", and a meter that went quiet when you muted something would be a
-- second volume display.
--
function audio.streams()
  local r = fs.send("/dev/audio", { type = "streams" })

  if not r then return nil, "no audio server" end

  return r.streams or {}, r.master or 256
end

--
-- Gain and balance are integers: 256 is unity, balance is -100 to +100.
--
-- Not floats, and the reason is in `sys.mix`: the mixing is `(sample *
-- gain) >> 8`, which is exact at unity. A volume control that is exact at
-- unity cannot quietly attenuate a stream nobody asked it to touch.
--
function audio.set(what)
  return fs.send("/dev/audio", what and
                 { type = "set", stream = what.stream, gain = what.gain,
                   balance = what.balance, muted = what.muted,
                   master = what.master } or { type = "set" })
end

return audio
