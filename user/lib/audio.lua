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

local OP_OPEN, OP_CLOSE, OP_SET, OP_STREAMS = 1, 2, 3, 4

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
  --
  -- Bounded, because the alternative is a program that never returns.
  --
  -- This waited for ever. If the server stops draining the ring - because
  -- it died, and a server spawned with one capability dies *silently* -
  -- then every client blocks in here with nothing on screen and nothing on
  -- the serial line. That is indistinguishable from a hung machine, and it
  -- cost an evening of suspecting the test harness.
  --
  -- Two seconds is far longer than any legitimate wait: the ring plus the
  -- device is well under a hundred milliseconds of sound, so anything past
  -- that is not back pressure, it is a fault.
  --
  for _ = 1, 500 do
    if sys.ring_put(self.ring, pcm) then return true end

    -- One tick. The ring holds several periods and the device a few more,
    -- so there are tens of milliseconds still to play while this sleeps.
    sys.sleep(1)
  end

  return false, "the audio server stopped taking periods"
end

-- How many periods are waiting to be mixed, and how much room is left.
function stream:queued()
  return sys.ring_ready(self.ring) or 0
end

function stream:space()
  return sys.ring_space(self.ring) or 0
end

--
-- Where this stream has actually got to, in frames out of the speaker.
--
-- Not the same as what has been handed over, and the gap between the two is
-- the point: the ring holds periods the server has not taken yet and the
-- device holds periods it has taken and not played. A position built from
-- what this process knows would be ahead by both of them - about seventy
-- milliseconds - which is enough to put a progress bar visibly wrong and
-- far too much to sync anything to.
--
-- The server publishes it into this stream's own ring, so reading it is a
-- load rather than a round trip, and `music` can ask on every pass without
-- thinking about it.
--
function stream:position()
  local played = sys.ring_position(self.ring)

  return played or 0
end

--
-- How many frames between a sample written now and that sample being heard.
--
-- This is the number to divide by `self.rate` for a latency in seconds, and
-- it is the honest one: it counts the ring and the device together, because
-- it is a subtraction of two frame counts rather than a sum of two things
-- somebody has to remember to add.
--
-- Zero before anything has been written, and zero rather than negative if
-- the two halves are read across an update.
--
function stream:delay()
  local played, written = sys.ring_position(self.ring)
  local ahead = (written or 0) * self.frames - (played or 0)

  if ahead < 0 then return 0 end

  return ahead
end

--
-- The worst latency this stream can ever have, in frames.
--
-- The ring plus the device, because those are the only two places a frame
-- can be waiting. `delay` is what the latency *is* at this instant; this is
-- what it cannot exceed, and the gap between them is slack the client has
-- chosen not to use.
--
-- Worth having as a number rather than a comment: it is what a caller
-- checks its own `delay` against, and a delay past the bound means the
-- position and the indices disagree rather than that the machine is busy.
--
function stream:bound()
  return (self.ring_periods + self.periods) * self.frames
end

--
-- Declared here and defined in the protocol block below, because `open` and
-- `close` need it and the layout it depends on reads better next to the
-- rest of the wire format than scattered up here.
--
local request

function stream:close()
  request(OP_CLOSE, { stream = self.id })

  --
  -- The ring is this process's memory and the server only borrowed a view.
  -- It unmaps and drops its own name for it on `close`; this drops ours.
  --
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
--
-- `periods` is how deep the ring should be, and it is the client's call
-- because the right answer is different for different clients.
--
-- **A shallow ring is low latency and a deep one is safe, and nothing can
-- be both.** A game wants the sound to arrive with the frame that caused
-- it and would rather risk a gap; a video player wants a clock it can hang
-- pictures on and would far rather be 200 ms behind than skip - a gap
-- desynchronises everything after it, where latency is a constant you
-- schedule against. One number cannot serve both, so it stops being a
-- constant.
--
-- Omitted means the default in `audioring.h`, which is what every caller
-- got before this argument existed.
--
function audio.open(name, periods)
  local fmt = audio.format()

  if fmt.period == 0 then return nil, "this machine has no sound device" end

  local cap, at = sys.ring_create(fmt.period, periods)

  if not cap then return nil, tostring(at) end

  -- The capability goes as `send`'s third argument, which is how a window
  -- hands the compositor its surface. It is not a field in the message: a
  -- capability is not data and does not travel as any.
  local r, why = request(OP_OPEN, { name = tostring(name or "sound") }, cap)

  if not r then
    sys.release(cap)

    return nil, tostring(why)
  end

  -- `frames` and `rate` are carried because `delay` is in frames and a
  -- caller wanting milliseconds should not have to ask the machine again
  -- for what this function already had in its hand.
  --
  -- `periods` is the *device's* depth, which is what the server replies
  -- with; `ring_periods` is this ring's, which is a different number and
  -- now a different number per stream.
  local s = setmetatable({ id = r.stream, period = r.period,
                           periods = r.periods, ring = at, cap = cap,
                           frames = fmt.frames, rate = fmt.rate }, stream)

  --
  -- Asked of the ring rather than taken from what was requested.
  --
  -- `ring_create` has limits of its own and quietly uses the default when
  -- it does not like a number, so a client that believed its own argument
  -- would compute a bound that was wrong and never find out. Nothing has
  -- been written yet, so what is free is the whole ring.
  --
  s.ring_periods = s:queued() + s:space()

  return s
end

--------------------------------------------------------------------------
-- Talking to the server, which is C and reads a struct.
--
-- **The layout below is `audioproto.h` written a second time**, and there is
-- no way around it: the server is C and this is Lua, so one holds the shape
-- as a struct and the other as a format string. What can be done is to make
-- a disagreement loud instead of silent - the sizes are asserted here when
-- this library loads, and the server refuses any request that is not
-- exactly the length it expects.
--
-- `serialize.h` argues at length that one implementation on both sides is
-- the point rather than a convenience, and it is right. This is the one
-- place that rule is bent, deliberately, because the alternative is an
-- interpreter inside a server that owes the device a period every 5.8 ms.
-- A check stands where the shared implementation used to.
--------------------------------------------------------------------------

-- struct audio_request: op, stream, gain, balance, muted, master, name[24]
local REQUEST  = "<I4I4i4i4i4i4c24"
local REQ_SIZE = 48

-- struct audio_stream_info: stream, gain, balance, muted, peak, queued, name
local INFO      = "<I4I4i4I4I4I4c24"
local INFO_SIZE = 48

-- struct audio_reply, as far as the list
local REPLY      = "<I4I4I4I4I4I4I4I4I4"
local REPLY_HEAD = 36

assert(#string.pack(REQUEST, 0, 0, 0, 0, 0, 0, "") == REQ_SIZE,
       "audio: the request layout does not match audioproto.h")
assert(#string.pack(INFO, 0, 0, 0, 0, 0, 0, "") == INFO_SIZE,
       "audio: the stream layout does not match audioproto.h")

--
-- An error is a number on the wire and a sentence here.
--
-- A real consequence of a struct protocol, and worth naming rather than
-- hiding: the server detects the fault, this decides what to call it, so
-- the words live with whoever shows them to a person and the server never
-- composes a string.
--
local ERRORS = {
  [1] = "this machine has no sound device",
  [2] = "no such stream",
  [3] = "that is not an audio ring",
  [4] = "too many streams already",
  [5] = "the audio server did not understand that",
}

function request(op, fields, pass)
  local bytes = string.pack(REQUEST, op,
                            fields.stream or 0,
                            fields.gain or -1,
                            fields.balance or -101,
                            fields.muted or -1,
                            fields.master or -1,
                            fields.name or "")

  local reply, why = fs.raw("/dev/audio", bytes, pass)

  if not reply then return nil, tostring(why) end

  if #reply < REPLY_HEAD then
    return nil, "the audio server sent a reply of the wrong size"
  end

  local err, stream, period, periods, master, mixes, starved, late, count =
      string.unpack(REPLY, reply)

  if err ~= 0 then
    return nil, ERRORS[err] or ("audio error " .. tostring(err))
  end

  return { stream = stream, period = period, periods = periods,
           master = master, mixes = mixes, starved = starved,
           late = late, count = count, bytes = reply }
end

--
-- What is playing, for the Mixer and anything else that wants to know.
--
-- `peak` is from the last mix and is measured *before* gain, so a muted
-- stream that is still playing shows a moving meter - which is the question
-- a meter answers: who is sending audio.
--
function audio.streams()
  local r = request(OP_STREAMS, {})

  if not r then return {} end

  local out = {}

  for i = 1, r.count do
    local at = REPLY_HEAD + (i - 1) * INFO_SIZE + 1
    local id, gain, balance, muted, peak, queued, name =
        string.unpack(INFO, r.bytes, at)

    out[i] = { stream = id, gain = gain, balance = balance,
               muted = muted ~= 0, peak = peak, queued = queued,
               name = (name:gsub("%z.*$", "")) }
  end

  return out
end

--
-- What the server says about its own timekeeping.
--
-- `starved` is passes where the device had room and every ring was empty -
-- the clients are behind. `late` is the worst gap between two turns of the
-- server's loop - the server is behind. An underrun is one or the other,
-- and without both there is no telling which.
--
function audio.stats()
  local r = request(OP_STREAMS, {})

  if not r then return nil end

  return { starved = r.starved, late = r.late, mixes = r.mixes }
end

--
-- Change a gain, a balance, a mute, or the master.
--
-- Absent fields are left alone, which is why they are signed on the wire: a
-- `set` that had to send every value would make the Mixer read the server's
-- state back before moving one fader, and two things that must agree is how
-- this system keeps hurting itself.
--
function audio.set(what)
  local r, why = request(OP_SET, {
    stream = what.stream,
    gain = what.gain,
    balance = what.balance,
    muted = (what.muted ~= nil) and (what.muted and 1 or 0) or nil,
    master = what.master,
  })

  if not r then return false, why end

  return true
end

return audio
