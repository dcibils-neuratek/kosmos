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
function stream:play(pcm)
  local r, why = fs.send("/dev/audio", { type = "play", stream = self.id,
                                         pcm = pcm })

  if not r then return false, tostring(why) end

  return r.taken and true or false, r.taken and nil or "full"
end

--
-- Hand over a period and do not come back until it is taken.
--
-- `play` reports "full" and returns, which is what something with other
-- work to do wants - Doom has a frame to render and cannot sit here. But a
-- program whose whole job is playing a file has nothing else to do, and
-- what it did instead was call `play` again immediately, for ever, at
-- whatever priority it happened to have.
--
-- That cost 63% of the machine, and it was worse than the number suggests:
-- a program launched from the window manager is in the *display* band and
-- the audio server is in *normal*, so the client spinning on "full"
-- outranked the server that would have made room. The thing waiting was
-- preempting the thing it waited for.
--
-- So the wait lives here rather than in each program, because it is the
-- kind of mistake that is invisible until somebody looks at a meter.
--
function stream:write(pcm)
  while true do
    local took, why = self:play(pcm)

    if took then return true end
    if why ~= "full" then return false, why end

    --
    -- One tick. The server holds a queue of `self.periods`, so there is
    -- most of that still to play while this sleeps, and being woken with
    -- room to spare is the whole idea.
    --
    sys.sleep(1)
  end
end

function stream:close()
  fs.send("/dev/audio", { type = "close", stream = self.id })
end

--
-- A stream, named for the Mixer.
--
-- The name is the program's own and nothing checks it: two copies of the
-- same program get two streams with the same name, which is right - they
-- are two things making a noise and each should have its own fader.
--
function audio.open(name)
  local r, why = fs.send("/dev/audio", { type = "open", name = name })

  if not r then return nil, tostring(why) end

  return setmetatable({ id = r.stream, period = r.period,
                        periods = r.periods }, stream)
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
