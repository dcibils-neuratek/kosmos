# The Playback Kit: a song's and a film's sound on a thread of its own

Agreed on 10 November 2026 (Diego: "yes, build the C playback thread for
music and video"), from `roadmap.md` 6zw: `x86-film`'s gaps under the
gate's load are not a test's fault but the player's. This is the design,
written before the code; its diagram is `playback-architecture.png`, drawn
from `playback-architecture.html`.

## What is wrong now

A song (`media.lua`) and a film's sound (`video.lua`'s voice) are fed to the
audio server the same way: `media.feed`, in Lua, from the program's own loop.
Each pass reads a piece of the file, decodes it (the MP3 Kit, the AAC Kit),
appends the samples to a Lua string, converts them with `sys.pcm` into
another string, and hands a period at a time to the stream's ring with
`sys.ring_put` - a third string a period. The ring holds 32 periods for a
film (186 ms) and 8 for a song (46 ms).

So the sound is exactly as punctual as the window's loop:

- **a loop held away longer than the ring** - a picture decoding, a garbage
  collection, a busy machine - leaves the device nothing, and it plays zeros.
  That is the six `x86-film` failures since 29 September, and it is what a
  film on a busy desktop does too;
- **the period path allocates**: three strings a period, about 500 KB a
  second for a film - the shape `CLAUDE.md` records the audio server was
  rewritten to escape;
- **nothing of it is in the audio band.** Groove is the only program whose
  sound comes from a thread there, because the Synth Kit has one.

## What it does

`/Kosmos/Kits/playback`, C, in `user/kits/playback/`. A song or a film's
sound is given to it as **compressed frames**, and it plays them: decodes,
converts to the device's format, and writes straight into the stream's ring,
on a thread of its own in the audio band. The window reads the file and asks
where the sound is; it is never between a frame and the speaker.

- **WAV, MP3 and AAC** - the three a song or a film can carry today; WAV's
  frames are samples already.
- **Play, pause, seek, volume, the end**, as `media.lua`'s player has them
  now, so Music and Video do not change what they call.
- **A position from the sound** - frames out of the speaker, as now.
- **What it heard**, for the LLM Monitor's kind of honesty: periods written,
  how often the ring was found empty, the longest the thread was away, and
  whether it is in the band - `x86-film` reads these.

## The two rings

**It cannot read the file itself.** A read is a request to `/Home` or
`/Drives`, and a thread in the audio band waits on no server (`CLAUDE.md`,
*Nothing waits on hardware, and nothing on the desktop waits on a server*).
So there are two rings, each with one writer and one reader:

```
  window (Lua)                     playback thread (C, audio band)       audio server
  fs.read_into ──► INPUT RING ──►  decode ─► convert ─► ┐
  (compressed frames,              (MP3 / AAC Kit,      ├──► AUDIO RING ──► device
   seconds ahead)                   sys.pcm's resampler)┘    (periods)
```

- **The input ring** is a region the kit makes: compressed frames, each a
  record `{ uint32 bytes; uint32 flags; }` followed by its bytes, padded to
  eight. The window writes, the thread reads, the indices are the only
  thing both touch (`audioring.h`'s discipline, between two threads).
  **One megabyte**: a minute of a 128 kb/s song, half a minute of most
  films' sound - so the window may be away for seconds and nothing is
  heard. A record never wraps: one that would not fit before the end writes
  a skip record there and starts again at the front.
- **The audio ring** is the stream's, as `audio.open` makes it today; the
  thread writes periods into its slots in place, as the Synth Kit does. A
  film's stays 32 periods; a song's 8.

What crosses each way, and nothing else:

- **frames**, window to thread, in the input ring;
- **commands** - play, pause, seek (a new stream's ring and a fresh start),
  volume, close - in a ring of 64 fixed records, as the Synth Kit's, the
  window writing and the thread reading;
- **what is heard**, thread to window, published after each period under a
  sequence number and read as a copy.

A seek is a **generation**: the window bumps it with the new ring, drops
what it has not written, and starts writing from the new place; the thread,
seeing it, resets its decoder and resampler and skips any record of the old
generation still in the input ring.

## The thread's pass

```
loop:
  take commands
  if playing:
    while the audio ring has room and the device's format allows:
      while fewer than a period of converted samples:
        if the input ring is empty: break            (heard.dry++ if the audio ring is short)
        decode the next frame into the PCM buffer   (C, no allocation)
        convert to the device: rate, channels       (sys.pcm's resampler, its phase kept)
      write a period into the ring's next slot; publish
  publish what is heard
  sleep one tick when two periods are queued, as the Synth Kit does
```

Everything it holds is allocated at `start` and freed at `close`: the
decoder, a frame's PCM (`AAC_ROOM` int16, or MP3's 1152 x 2), the converted
remainder, the input ring. The pass allocates nothing.

## What supplies each piece

| Piece | Supplied by | New? |
|---|---|---|
| The thread, in the audio band | `kosmos_thread_start_audio`, as Groove | no |
| The ring to the audio server | `audioring.h`, `audio.open` | no |
| MP3 decoding | `minimp3.h`, the MP3 Kit's | no - its core moved to a `.c` both kits link |
| AAC decoding | `aac_core.h` | no |
| Converting to the device | `sys.pcm`'s resampler | **moved** from `sys_user.c` to `user/lib/c/pcm.c`, one copy that `sys.pcm` and this kit both call |
| The input ring | this kit | yes |
| Demuxing an MP4, finding a song's frames | `video.lua`, `media.lua`, as now | no |
| The clock the picture follows | `stream:position()`, as now | no |

**Does another application want this?** Every program that plays a file:
Music and Video now, and anything that later plays a file's sound - a
game's music, a voice. That is why it is a kit and has one door. **Does something already supply it?** The Synth Kit has the thread's
shape and is kept separate: it renders sound from a song's notes and has no
input; this decodes sound that exists. What they share - the band, the
ring, the depth rule - is in headers both include (`audioring.h`,
`depth.h`), not copied.

## The door

```lua
local playback = use("/Kosmos/Kits/playback")
local p, why = playback.open{ kind = "aac", config = esds, rate = 44100,
                              channels = 2, ring = stream.ring }   -- or "mp3", "wav"
p:room()                -- bytes the input ring can take now
p:frame(at, bytes)      -- a frame from a region's address: true, or false when full
p:frame_string(s)       -- the same from a string (WAV's samples, tests)
p:ended()               -- no more frames will come
p:play()  p:pause()  p:volume(level)
p:seek(stream.ring)     -- a new stream's ring, and everything before forgotten
p:state(t)              -- what is heard, into t: written, dry, away_max_us,
                        -- audio_band, finished, frames_out
p:close()
```

`media.lua` keeps its player's face - `open`, `play`, `tick`, `seek`,
`position`, `volume`, `peak`, `finished`, `close` - and `tick` changes from
*feed the server* to *keep the input ring full*: read the next piece of the
file and hand over its frames while the ring has room. `video.lua`'s voice
the same, with the MP4's samples as the frames. `media.feed` and the
`decoded`/`pending` strings go. A machine built `FULL=0` has no AAC: the kit
says so at `open`, as `video.lua` does now.

## The busiest path

A film at 44.1 kHz stereo, AAC at 128 kb/s, on the thread: 43 frames a
second, each about 370 bytes in and 4 KB out; 172 periods a second written.
How long a frame's decode and a period's conversion take on the M700 is
not measured yet; step 3 measures them, on the thread, under `x86-film`,
and the M700 when it is next netbooted. The window's part drops from every period to a read of 256 KB every few
seconds.

## Steps

1. **The resampler and MP3's core shared** - `pcm.c` from `sys_user.c`,
   `sys.pcm` calling it, held byte for byte by a host test against the old
   path's output; `minimp3`'s implementation in one `.c`.
2. **The kit on the Mac** - the input ring, decode, convert, write into an
   `audio_ring` in memory; `tools/test_playback.c` plays `al05_44` and an
   MP3 through it and holds the periods to the Lua path's, sample for
   sample, with a seek and an end.
3. **The thread** - in the audio band, its commands and what it heard;
   `video.lua`'s voice on it. `x86-film` as it is, and then **with every
   core busy** - spinners in the guest, as 18.263 measured Groove's - which
   fails today and must pass. Control: the thread started outside the band.
4. **Music on it** - `media.lua`'s player; `x86-sound` and the Music suites.
5. **`media.feed` gone**, and nothing left that hands sound over a string
   at a time but tones and beeps made in Lua.

Each step's suites pass before the next; the whole gate after step 4, since
`media.lua` and `sys.pcm` are shared.
